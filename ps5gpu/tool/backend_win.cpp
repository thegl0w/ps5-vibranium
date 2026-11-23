// SPDX-License-Identifier: MIT
// Backend over ps5gpu.sys.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <emmintrin.h>
#include "backend.h"
#include "../include/ps5gpu_ioctl.h"

static HANDLE gDev = INVALID_HANDLE_VALUE;
static uint32_t gRefused;

namespace be
{
    // One mutex for every program that uses the GPU. A holder that died leaves it "abandoned": the next one gets it
    // (the shared state may then be half-updated; lib/ps5drm.cpp checks what it follows).
    //
    // The GPU's programs run as different users (dwm.exe as DWM-n, programs as the desktop user, the watchdog's tools as
    // SYSTEM), so the mutex is created with a security descriptor that lets everyone wait on it: a NULL DACL and a
    // low mandatory label. The first version used default security: whoever created it first locked the others out,
    // and they then ran WITHOUT the lock, two programs changed the heap at once and got overlapping memory (the
    // compositor's textures showed other programs' pictures, 2026-11-21). Hence the new name, and never without it.
    static HANDLE gMutex;
    static HANDLE openMutex()
    {
        static const char* const kName = "Global\\ps5gpu-state2";
        SECURITY_DESCRIPTOR sd;
        SECURITY_ATTRIBUTES sa = { sizeof sa, &sd, FALSE };
        bool sdOk = InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) && SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
        // Low mandatory label (no write-up restriction for low-integrity programs): S-1-16-4096, policy NO_WRITE_UP.
        alignas(8) unsigned char saclBuf[64];
        PACL sacl = (PACL)saclBuf;
        alignas(8) unsigned char sidBuf[SECURITY_MAX_SID_SIZE];
        DWORD sidLen = sizeof sidBuf;
        if (sdOk && InitializeAcl(sacl, sizeof saclBuf, ACL_REVISION) && CreateWellKnownSid(WinLowLabelSid, NULL, (PSID)sidBuf, &sidLen) &&
            AddMandatoryAce(sacl, ACL_REVISION, 0, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, (PSID)sidBuf))
            SetSecurityDescriptorSacl(&sd, TRUE, sacl, FALSE);
        HANDLE m = CreateMutexA(sdOk ? &sa : NULL, FALSE, kName);
        if (!m) m = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, kName);
        if (!m) {
            char msg[160];
            snprintf(msg, sizeof msg, "ps5gpu: cannot open the GPU state mutex (error %lu): no GPU for this program\n", GetLastError());
            OutputDebugStringA(msg);
            fputs(msg, stderr);
        }
        return m;
    }
    bool envFlag(const char* name)
    {
        char v[16];
        DWORD n = GetEnvironmentVariableA(name, v, sizeof v);
        return n > 0 && n < sizeof v && strcmp(v, "0") != 0;
    }
    bool lockReady()
    {
        if (!gMutex) {
            HANDLE m = openMutex();
            if (m && InterlockedCompareExchangePointer((PVOID*)&gMutex, m, NULL) != NULL) CloseHandle(m);
        }
        return gMutex != NULL;
    }
    void lock()
    {
        if (!lockReady()) {                              // never touch the shared state unlocked: stop here instead
            fputs("ps5gpu: no GPU state lock, stopping\n", stderr);
            TerminateProcess(GetCurrentProcess(), 0xC0000022);
        }
        WaitForSingleObject(gMutex, INFINITE);
    }
    void unlock() { ReleaseMutex(gMutex); }
    uint32_t pid() { return GetCurrentProcessId(); }
    uint64_t bootStamp()
    {
        // When Windows started, to 16 s: wall clock minus uptime (both in 100 ns units), rounded so that every program
        // computes the same value. The +1 keeps it from ever being 0.
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        uint64_t now = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime, up = GetTickCount64() * 10000ull;
        return ((now - up + 80000000ull) / 160000000ull) + 1;
    }
    bool isCompositor()
    {
        char exe[MAX_PATH] = "";
        GetModuleFileNameA(NULL, exe, sizeof exe - 1);
        const char* base = strrchr(exe, '\\');
        return !_stricmp(base ? base + 1 : exe, "dwm.exe");
    }

    bool pidAlive(uint32_t pid)
    {
        if (pid == GetCurrentProcessId()) return true;
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return GetLastError() == ERROR_ACCESS_DENIED;      // exists, but not ours to open
        bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
        CloseHandle(h);
        return alive;
    }

    bool open()
    {
        if (gDev != INVALID_HANDLE_VALUE) return true;
        gDev = CreateFileA(PS5GPU_WIN32_PATH, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (gDev == INVALID_HANDLE_VALUE) {
            printf("cannot open %s: error %lu\n", PS5GPU_WIN32_PATH, GetLastError());
            return false;
        }
        PS5GPU_INFO info = {};
        DWORD n = 0;
        if (!DeviceIoControl(gDev, PS5GPU_IOCTL_INFO, NULL, 0, &info, sizeof info, &n, NULL) || !(info.Flags & PS5GPU_F_FOUND)) {
            printf("driver has no GPU (flags 0x%x)\n", info.Flags);
            return false;
        }
        if (info.Flags & PS5GPU_F_VM) {
            printf("this is the VM rehearsal driver; stage 2 is for the console only\n");
            return false;
        }
        return true;
    }

    bool arm(bool on)
    {
        uint32_t magic = on ? PS5GPU_ARM_MAGIC : PS5GPU_DISARM_MAGIC;
        DWORD n = 0;
        return DeviceIoControl(gDev, PS5GPU_IOCTL_ARM, &magic, sizeof magic, NULL, 0, &n, NULL) != 0;
    }

    static bool one(uint32_t kind, uint32_t reg, uint32_t& value)
    {
        PS5GPU_REGOP op = { kind, reg, value, 0, 0 };
        DWORD n = 0;
        if (!DeviceIoControl(gDev, PS5GPU_IOCTL_REGS, &op, sizeof op, &op, sizeof op, &n, NULL) || !(op.Op & PS5GPU_OP_DONE)) {
            ++gRefused;
            return false;
        }
        value = op.Value;
        return true;
    }

    bool rd(uint32_t reg, uint32_t& value)
    {
        value = 0;
        return one(PS5GPU_OP_READ, reg, value);
    }

    bool wr(uint32_t reg, uint32_t value) { return one(PS5GPU_OP_WRITE, reg, value); }

    void* map(uint64_t phys, uint64_t bytes, bool writeCombining)
    {
        PS5GPU_MAP m = {};
        DWORD n = 0;
        m.Phys = phys;
        m.Bytes = bytes;
        m.Flags = writeCombining ? PS5GPU_MAP_WC : 0;
        if (!DeviceIoControl(gDev, PS5GPU_IOCTL_MAP, &m, sizeof m, &m, sizeof m, &n, NULL)) {
            printf("map 0x%llx + 0x%llx failed: error %lu\n", (unsigned long long)phys, (unsigned long long)bytes, GetLastError());
            return NULL;
        }
        return (void*)(uintptr_t)m.UserVa;
    }

    void unmap(void* va)
    {
        PS5GPU_MAP m = {};
        DWORD n = 0;
        m.UserVa = (uint64_t)(uintptr_t)va;
        DeviceIoControl(gDev, PS5GPU_IOCTL_UNMAP, &m, sizeof m, NULL, 0, &n, NULL);
    }

    uint64_t nowUs()
    {
        static LARGE_INTEGER freq;
        LARGE_INTEGER t;
        if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t);
        return (uint64_t)(t.QuadPart * 1000000.0 / freq.QuadPart);
    }

    void delayUs(uint32_t us)
    {
        if (us >= 5000) {
            Sleep(us / 1000);
            return;
        }
        uint64_t end = nowUs() + us;
        while (nowUs() < end) {}
    }

    void flushWrites() { _mm_sfence(); }

    bool doorbell64(uint32_t byteOffset, uint64_t value)
    {
        static volatile uint8_t* bar2;
        if (!bar2) {
            PS5GPU_INFO info = {};
            DWORD n = 0;
            if (!DeviceIoControl(gDev, PS5GPU_IOCTL_INFO, NULL, 0, &info, sizeof info, &n, NULL) || info.Bar[2] == 0) return false;
            bar2 = (volatile uint8_t*)map(info.Bar[2], 0x10000, false);
            if (!bar2) return false;
        }
        if (byteOffset > 0x10000 - 8 || (byteOffset & 7)) return false;
        *(volatile uint64_t*)(bar2 + byteOffset) = value;
        return true;
    }

    uint32_t refusedCount() { return gRefused; }
}
