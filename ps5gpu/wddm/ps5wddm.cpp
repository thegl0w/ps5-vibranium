// SPDX-License-Identifier: MIT
// ps5wddm.sys, stage W1: a WDDM display-only miniport for the PS5's GPU (PCI 1002:13FC).
//
// What it does: takes the screen over from Windows' basic display driver in the mode the firmware left (no mode set,
// no link training, no display register is written), offers that one mode to Windows, and shows what Windows presents
// by copying it into the frame buffer. No GPU acceleration here: this stage proves the driver package, the start-up
// with dxgkrnl and the way back to the basic driver. The GPU itself is still reached through ps5gpu.sys.
//
// The way back, without a keyboard: before anything else StartDevice writes StartPending=1 to the driver's service
// key and flushes it. The value is cleared after kPresentsToTrust presents, or the first present 15 s after the start (and on a clean stop). If a start finds
// it set, the previous start crashed or hung: this start fails, and Windows uses its basic display driver again.
// Clear it by hand (reg add HKLM\SYSTEM\CurrentControlSet\Services\ps5wddm /v StartPending /t REG_DWORD /d 0 /f).
// Progress breadcrumbs go to the same key (Stage, Width, Height, Pitch, PhysLow, PhysHigh, LastStatus, Presents).
//
// Stage W3 (2026-11-20): the same binary can also register as a FULL graphics driver (WDDM 1.3), chosen at load by the
// registry value FullDriver=1 in the service key (0 or absent: display-only, exactly as before). The full mode follows
// the shape of the open viogpu3d driver (virtio-win, kvm-guest-drivers-windows): one dummy aperture segment, so every
// allocation is system memory that Windows hands us page by page; nothing is rendered in the kernel. What it does:
//   - keeps the allocations Windows creates (shared primary, shadow, staging, and later the ones our Direct3D
//     user-mode driver creates) and maps the ones in the aperture so they can be read,
//   - shows the allocation Windows points the screen at (SetVidPnSourceAddress) by copying it into the frame buffer
//     60 times a second from a thread, and reports each such tick as the vertical sync,
//   - carries out "present" copies and fills between allocations with the CPU, in submission order, and reports every
//     submitted buffer as completed (the GPU work itself is done by the user-mode driver through Vulkan).
// Progress and failures go to a text log in the service key (value Log, rewritten every 2 s while it changes).
//
// GPU start at boot (2026-11-22): DriverEntry can start the GPU itself (gpuboot.cpp, the steps of `ps5gpu2 run`) and
// then register as the full driver straight away, so the screen no longer blinks when autostart.cmd switches modes.
// Any problem: display-only as before, reason in GpuBootReason. See "GPU start at boot" near the end of this file.
//
// Built against WDDM 1.3 (DXGKDDI_INTERFACE_VERSION 0x4002); the display-only registration still declares Windows 8.
extern "C" {
#include <ntddk.h>
#include <dispmprt.h>
#include <ntstrsafe.h>
}
#include <stdarg.h>
#include "gpuboot.h"

#define P5_TAG '5wSP'
static const ULONG kTickets = 1024;
static const ULONG kPresentsToTrust = 30;            // or the first present 15 s after the start (an idle screen presents rarely)

struct Device {
    DEVICE_OBJECT* pdo;
    DXGKRNL_INTERFACE dxgk;
    DXGK_DISPLAY_INFORMATION disp;
    UCHAR* fb;
    SIZE_T fbBytes;
    ULONG width, height, pitch;
    ULONG refreshNum, refreshDen;        // the display's refresh rate as a fraction (60/1, 120000/1001, ...); see MeasureRefresh
    ULONG refreshMilliHz;                // as measured (0 = not measured, 60 Hz assumed)
    BOOLEAN started;
    BOOLEAN ownsDisplay;
    BOOLEAN trusted;
    volatile LONG presents;
    LARGE_INTEGER startTime;
    KEVENT stopEvent;
    PETHREAD trustThread;
    volatile ULONG* regs;                // the GPU's register block (BAR5), only on the PS5's GPU
    ULONG regDwords;
    volatile LONG irqCount;
    ULONG irqLastMessage;
    ULONG refused;
    // our own interrupt ring (stage W2b), in the GPU-memory area ps5gpu2 also uses
    volatile UCHAR* ihRing;
    ULONGLONG ihMc, ihPhys;
    ULONG ihRptr;
    BOOLEAN ihOn;
    volatile LONG ihEntries;
    ULONG ihOverflows;
    struct { ULONG key; LONG count; } ihStat[8];
    // ---- full-driver mode ----
    BOOLEAN full;
    ULONGLONG fbPhys;                    // the frame buffer we were handed, as the CPU reaches it (through the GPU's memory window)
    ULONGLONG fbScan;                    // ... and as the display reads it (address in the GPU's memory)
    ULONGLONG segPhys, segCpu, segBytes; // the GPU-memory range Windows places allocations in: display address, CPU address, size
    UCHAR* segVa;
    UCHAR* scanPage;                     // the page of our area that holds libps5gpu's screen record
    struct ScanRec* scanRec;
    ULONG pipes;                         // display pipes that show the picture
    BOOLEAN flipOk;                      // the display can be pointed at an allocation
    ULONGLONG shownPhys;
    ULONG pagingSegment, pagingInsufficient;
    ULONG flipsDone, flipsHeld, skipped, patches, submits, pageIns, pageOuts, pageMoves, pageFills, pageFails;
    struct { ULONG id; PFN_NUMBER* pfns; } tickets[kTickets]; // page-number copies and GDI command blobs waiting for the engine thread (queueLock)
    ULONG nextTicket;
    struct Alloc* volatile source;       // what Windows wants on the screen
    PHYSICAL_ADDRESS sourceAddress;
    volatile LONG sourceChanged, visible, vsyncOn, notifyPending;
    PETHREAD engine;
    KEVENT engineWake;
    KSPIN_LOCK queueLock;
    struct Work* queue;                  // kQueue entries, a ring
    ULONG queueHead, queueTail;
    volatile LONG lastSubmitted, lastCompleted;
    LONG allocs, devices, contexts;
    ULONG presents3, blts, fills, flips, renders, vsyncs, copies, preempts, dropped;
    ULONG renderKms, gdiOps[8], gdiSkips, gdiNoTicket, apMaps, apUnmaps, apFails;
};

struct Tr {                                      // full mode: an entry point's way in and way out, into the trace file
    const char* name;
    BOOLEAN on;
    Tr(const char* n);
    ~Tr();
};
static void TraceOpen();
static void TraceClose();
static void LogFlush();
extern "C" NTSYSAPI NTSTATUS NTAPI ZwFlushBuffersFile(HANDLE file, PIO_STATUS_BLOCK io);      // ntifs.h has it; this file builds on ntddk.h
static void StartEngine(Device* d);
static void StopEngine(Device* d);
static BOOLEAN SetupVram(Device* d);
static void ReleaseVram(Device* d);
static void FlipTo(Device* d, ULONGLONG phys);
static void ShowFrameBuffer(Device* d);
static void LogF(const char* fmt, ...);
static BOOLEAN gBootStarted;             // this load of the driver started the GPU itself in DriverEntry (full mode follows)
static void BootSettle(BOOLEAN trusted, const char* why);

static UNICODE_STRING gRegPath;
static BOOLEAN gFull;                    // registered as a full graphics driver (registry value FullDriver=1)
static ULONG gGdi;                       // CDD/DWM interop: GDI surfaces in the segment, drawn by Windows on the CPU (volatile Session\GdiInterop: 1 on, 3 on without software device bitmaps)
static ULONG gGdiTexCpu = 1;             // GDI TEXTURE surfaces CPU-visible (Session\GdiTexCpu, default 1)
static ULONG gGdiAccel;                  // kernel-mode GDI acceleration (RenderKm, GDI surfaces, aperture segment); volatile Session\GdiAccel while tested
static BOOLEAN gWddm2;                   // ... as WDDM 2.0 instead of 1.3 (registry value FullVersion=20; crashed the console 2026-11-20)
static WCHAR gRegPathBuf[260];

// ---- registry: the start marker and breadcrumbs ----

static NTSTATUS RegOpen(HANDLE* key)
{
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &gRegPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    return ZwOpenKey(key, KEY_READ | KEY_WRITE, &oa);
}

static void RegSet(PCWSTR name, ULONG value, BOOLEAN flush)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    HANDLE key;
    if (!NT_SUCCESS(RegOpen(&key))) return;
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, name);
    ZwSetValueKey(key, &n, 0, REG_DWORD, &value, sizeof value);
    if (flush) ZwFlushKey(key);
    ZwClose(key);
}

static ULONG RegGet(PCWSTR name, ULONG fallback)
{
    HANDLE key;
    if (!NT_SUCCESS(RegOpen(&key))) return fallback;
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, name);
    struct { KEY_VALUE_PARTIAL_INFORMATION info; UCHAR more[8]; } buf;
    ULONG got = 0;
    ULONG value = fallback;
    if (NT_SUCCESS(ZwQueryValueKey(key, &n, KeyValuePartialInformation, &buf, sizeof buf, &got)) && buf.info.Type == REG_DWORD &&
        buf.info.DataLength == sizeof(ULONG))
        RtlCopyMemory(&value, buf.info.Data, sizeof value);
    ZwClose(key);
    return value;
}

static void Stage(ULONG n) { RegSet(L"Stage", n, FALSE); }

// "Full mode" is asked for in a volatile subkey (Services\ps5wddm\Session, value FullDriver=1; wddm-mode.cmd makes
// it): it is gone after a Windows restart, so the driver always comes up display-only at boot. Full mode needs the
// GPU started (the compositor's Direct3D driver renders through it), and that only happens after boot.
static ULONG RegGetSession(PCWSTR name, ULONG fallback)
{
    WCHAR path[300];
    if (!NT_SUCCESS(RtlStringCbPrintfW(path, sizeof path, L"%wZ\\Session", &gRegPath))) return fallback;
    UNICODE_STRING p, n;
    RtlInitUnicodeString(&p, path);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &p, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE key;
    if (!NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa))) return fallback;
    RtlInitUnicodeString(&n, name);
    struct { KEY_VALUE_PARTIAL_INFORMATION info; UCHAR more[8]; } buf;
    ULONG got = 0, value = fallback;
    if (NT_SUCCESS(ZwQueryValueKey(key, &n, KeyValuePartialInformation, &buf, sizeof buf, &got)) && buf.info.Type == REG_DWORD && buf.info.DataLength == sizeof(ULONG))
        RtlCopyMemory(&value, buf.info.Data, sizeof value);
    ZwClose(key);
    return value;
}

// ---- the GPU's registers (stage W2a) ----
// Same rule as ps5gpu.sys: nothing in the PSP/SMU block (dwords 0x16000-0x17FFF), and this driver does not use the
// indirect index/data pairs at all (dwords 0, 1, 0xC-0xF), so they are refused outright.
static BOOLEAN RegForbidden(Device* d, ULONG reg)
{
    return !d->regs || reg >= d->regDwords || (reg >= 0x16000 && reg <= 0x17FFF) || reg <= 1 || (reg >= 0xC && reg <= 0xF);
}
static ULONG RegRead(Device* d, ULONG reg)
{
    if (RegForbidden(d, reg)) { d->refused++; return 0xFFFFFFFF; }
    return READ_REGISTER_ULONG((PULONG)&d->regs[reg]);
}
static void RegWrite(Device* d, ULONG reg, ULONG value)
{
    if (RegForbidden(d, reg)) { d->refused++; return; }
    WRITE_REGISTER_ULONG((PULONG)&d->regs[reg], value);
}

// Register numbers (dword offsets in BAR5): OSSSYS block at 0x10A0 (osssys_5_0_0_offset.h, base index 0).
enum : ULONG {
    rIH_RB_CNTL = 0x10A0 + 0x80, rIH_RB_BASE = 0x10A0 + 0x81, rIH_RB_BASE_HI = 0x10A0 + 0x82, rIH_RB_RPTR = 0x10A0 + 0x83, rIH_RB_WPTR = 0x10A0 + 0x84,
    rIH_RB_CNTL_RING1 = 0x10A0 + 0x8C, rIH_RB_CNTL_RING2 = 0x10A0 + 0x98,
};
static const ULONG kIhRbEnable = 0x1, kIhEnableIntr = 0x20000;

// Linux leaves its interrupt ring running, and that ring lives in memory Windows owns now: an interrupt source that
// fires makes the GPU write there. Until today this was only closed when ps5gpu2 ran by hand. Same writes as
// ps5gpu2's step 1 (navi10_ih: ring and delivery off, pointers cleared; rings 1 and 2 off).
static void QuiesceInterruptRings(Device* d)
{
    ULONG found = RegRead(d, rIH_RB_CNTL);
    RegSet(L"IhCntlFound", found, FALSE);
    RegSet(L"IhBaseFound", RegRead(d, rIH_RB_BASE), FALSE);
    RegWrite(d, rIH_RB_CNTL, found & ~(kIhRbEnable | kIhEnableIntr));
    RegWrite(d, rIH_RB_RPTR, 0);
    RegWrite(d, rIH_RB_WPTR, 0);
    RegWrite(d, rIH_RB_CNTL_RING1, RegRead(d, rIH_RB_CNTL_RING1) & ~kIhRbEnable);
    RegWrite(d, rIH_RB_CNTL_RING2, RegRead(d, rIH_RB_CNTL_RING2) & ~kIhRbEnable);
    RegSet(L"IhCntlAfter", RegRead(d, rIH_RB_CNTL), FALSE);
}

// ---- our own interrupt ring (stage W2b; only when the registry value IhEnable is 1) ----
// The ring is 64 KiB of the GPU-memory carve-out (our 16 MiB area at top - 512 MiB, offset 0x40000: free in
// tool/layout.h), addressed by its GPU address like the command ring, so the GPU's writes can only land in memory
// that is ours. No write-back page, no doorbell: the write pointer is read from its register, the read pointer is
// written to its register, which also re-arms the interrupt (RPTR_REARM). Register values follow navi10_ih.c.
enum : ULONG {
    rIH_DOORBELL_RPTR = 0x10A0 + 0x87, rFB_LOCATION_BASE = 0x1260 + 0x1720, rFB_LOCATION_TOP = 0x1260 + 0x1721, rFB_OFFSET = 0x1260 + 0x170B,
};
static const ULONG kIhBytes = 0x10000, kIhMask = kIhBytes - 1;
static const ULONGLONG kAreaBelowTop = 0x20000000ull, kIhOffInArea = 0x40000;

static void StopInterruptRing(Device* d)
{
    if (!d->ihOn) return;
    d->ihOn = FALSE;
    RegWrite(d, rIH_RB_CNTL, RegRead(d, rIH_RB_CNTL) & ~(kIhRbEnable | kIhEnableIntr));
    RegWrite(d, rIH_RB_RPTR, 0);
    RegWrite(d, rIH_RB_WPTR, 0);
    if (d->ihRing) MmUnmapIoSpace((PVOID)d->ihRing, kIhBytes);
    d->ihRing = NULL;
}

static void StartInterruptRing(Device* d)
{
    ULONG fbBase = RegRead(d, rFB_LOCATION_BASE), fbTop = RegRead(d, rFB_LOCATION_TOP), fbOffset = RegRead(d, rFB_OFFSET);
    ULONGLONG mcBase = (ULONGLONG)(fbBase & 0xFFFFFF) << 24, mcTop = ((ULONGLONG)(fbTop & 0xFFFFFF) << 24) | 0xFFFFFF;
    ULONGLONG vram = mcTop - mcBase + 1, carve = (ULONGLONG)fbOffset << 24;
    if (carve < 0x100000000ull || vram < 0x40000000ull || vram > 0x400000000ull || mcTop < mcBase) { RegSet(L"IhFail", 1, FALSE); return; }
    d->ihMc = mcBase + vram - kAreaBelowTop + kIhOffInArea;
    d->ihPhys = carve + vram - kAreaBelowTop + kIhOffInArea;
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)d->ihPhys;
    d->ihRing = (volatile UCHAR*)MmMapIoSpace(pa, kIhBytes, MmWriteCombined);      // same caching as every other mapping of this memory
    if (!d->ihRing) { RegSet(L"IhFail", 2, FALSE); return; }
    RtlZeroMemory((PVOID)d->ihRing, kIhBytes);
    RegWrite(d, rIH_DOORBELL_RPTR, RegRead(d, rIH_DOORBELL_RPTR) & ~0x10000000u);
    ULONG cntl = RegRead(d, rIH_RB_CNTL);
    cntl &= ~(0x3Eu | 0x100u | kIhRbEnable | kIhEnableIntr | 0x70000000u);        // size, write-back, enables, address space
    cntl |= (14u << 1) | 0x40000000u | 0x80000000u | 0x10000u | 0x200000u;         // 2^14 dwords, GPU address space, overflow clear + enable, re-arm on rptr write
    RegWrite(d, rIH_RB_BASE, (ULONG)(d->ihMc >> 8));
    RegWrite(d, rIH_RB_BASE_HI, (ULONG)(d->ihMc >> 40) & 0xFF);
    RegWrite(d, rIH_RB_CNTL, cntl);
    RegWrite(d, rIH_RB_WPTR, 0);
    RegWrite(d, rIH_RB_RPTR, 0);
    d->ihRptr = 0;
    d->ihOn = TRUE;
    RegWrite(d, rIH_RB_CNTL, cntl | kIhRbEnable | kIhEnableIntr);
    RegSet(L"IhMcLow", (ULONG)d->ihMc, FALSE);
    RegSet(L"IhMcHigh", (ULONG)(d->ihMc >> 32), FALSE);
    RegSet(L"IhCntlOurs", RegRead(d, rIH_RB_CNTL), FALSE);
}

// Called from the DPC: take the new entries off the ring. Each entry is 8 dwords; the first holds client, source, ring.
static void DrainInterruptRing(Device* d)
{
    if (!d->ihOn || !d->ihRing) return;
    for (int pass = 0; pass < 4; ++pass) {
        ULONG w = RegRead(d, rIH_RB_WPTR);
        if (w & 1) {                                                                // overflow: skip to the newest entries, as Linux does
            d->ihOverflows++;
            d->ihRptr = (w + 32) & kIhMask & ~31u;
            RegWrite(d, rIH_RB_CNTL, RegRead(d, rIH_RB_CNTL) | 0x80000000u);
        }
        w &= kIhMask & ~31u;
        if (w == d->ihRptr) break;
        while (d->ihRptr != w) {
            ULONG key = READ_REGISTER_ULONG((PULONG)(d->ihRing + d->ihRptr)) & 0xFFFFFF;
            int slot = -1;
            for (int i = 0; i < 8; ++i) {
                if (d->ihStat[i].count && d->ihStat[i].key == key) { slot = i; break; }
                if (!d->ihStat[i].count && slot < 0) slot = i;
            }
            if (slot >= 0) { d->ihStat[slot].key = key; d->ihStat[slot].count++; }
            InterlockedIncrement(&d->ihEntries);
            d->ihRptr = (d->ihRptr + 32) & kIhMask;
        }
        RegWrite(d, rIH_RB_RPTR, d->ihRptr);
    }
}

static void MapRegisters(Device* d)
{
    ULONG id = 0, got = 0;
    if (!NT_SUCCESS(d->dxgk.DxgkCbReadDeviceSpace(d->dxgk.DeviceHandle, DXGK_WHICHSPACE_CONFIG, &id, 0, sizeof id, &got)) || id != 0x13FC1002) return;
    DXGK_DEVICE_INFO info;
    if (!NT_SUCCESS(d->dxgk.DxgkCbGetDeviceInformation(d->dxgk.DeviceHandle, &info)) || !info.TranslatedResourceList) return;
    PCM_FULL_RESOURCE_DESCRIPTOR full = &info.TranslatedResourceList->List[0];
    for (ULONG i = 0; info.TranslatedResourceList->Count && i < full->PartialResourceList.Count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = &full->PartialResourceList.PartialDescriptors[i];
        if (r->Type != CmResourceTypeMemory || r->u.Memory.Length != 0x80000) continue;      // BAR5: 512 KiB of registers
        PVOID va = NULL;
        if (NT_SUCCESS(d->dxgk.DxgkCbMapMemory(d->dxgk.DeviceHandle, r->u.Memory.Start, r->u.Memory.Length, FALSE, FALSE, MmNonCached, &va))) {
            d->regs = (volatile ULONG*)va;
            d->regDwords = r->u.Memory.Length / 4;
            RegSet(L"RegsPhysLow", r->u.Memory.Start.LowPart, FALSE);
        }
        break;
    }
}

// The refresh rate the screen really runs at (the mode Linux set before the hand-over: 60, 119.88, 120 Hz, ...), from
// OTG0's frame counter (OTG_STATUS_FRAME_COUNT, DCN 2.0.1: segment 2 + 0x1b4c, 24 bits) over about 250 ms. Snapped to
// the usual broadcast/PC rates when within 0.5 %, else reported as measured. Read-only; defaults to 60/1 when the
// registers are not there or the count does not move.
static void MeasureRefresh(Device* d)
{
    d->refreshNum = 60;
    d->refreshDen = 1;
    d->refreshMilliHz = 0;
    if (!d->regs || KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    const ULONG reg = 0x34C0 + 0x1b4c;
    LARGE_INTEGER freq, t0, t1;
    t0 = KeQueryPerformanceCounter(&freq);
    ULONG c0 = RegRead(d, reg) & 0xFFFFFF;
    LARGE_INTEGER wait;
    wait.QuadPart = -2500000LL;                              // 250 ms
    KeDelayExecutionThread(KernelMode, FALSE, &wait);
    ULONG c1 = RegRead(d, reg) & 0xFFFFFF;
    t1 = KeQueryPerformanceCounter(NULL);
    ULONG frames = (c1 - c0) & 0xFFFFFF;
    LONGLONG ticks = t1.QuadPart - t0.QuadPart;
    RegSet(L"RefreshFrames", frames, FALSE);
    if (frames == 0 || ticks <= 0 || c0 == 0xFFFFFF) return;
    ULONGLONG mhz = (ULONGLONG)frames * 1000ull * (ULONGLONG)freq.QuadPart / (ULONGLONG)ticks;
    if (mhz < 20000 || mhz > 250000) return;
    d->refreshMilliHz = (ULONG)mhz;
    static const struct { ULONG num, den; } kRates[] = {
        { 24, 1 }, { 25, 1 }, { 30, 1 }, { 30000, 1001 }, { 50, 1 }, { 60, 1 }, { 60000, 1001 }, { 75, 1 },
        { 100, 1 }, { 120, 1 }, { 120000, 1001 }, { 144, 1 }, { 165, 1 }, { 240, 1 },
    };
    ULONG bestNum = (ULONG)mhz, bestDen = 1000;
    ULONGLONG bestErr = mhz / 200;                           // 0.5 %
    for (ULONG i = 0; i < sizeof kRates / sizeof kRates[0]; ++i) {
        ULONGLONG r = (ULONGLONG)kRates[i].num * 1000ull / kRates[i].den;
        ULONGLONG err = r > mhz ? r - mhz : mhz - r;
        if (err <= bestErr) { bestErr = err; bestNum = kRates[i].num; bestDen = kRates[i].den; }
    }
    d->refreshNum = bestNum;
    d->refreshDen = bestDen;
    RegSet(L"RefreshMilliHz", d->refreshMilliHz, FALSE);
    RegSet(L"RefreshNum", bestNum, FALSE);
    RegSet(L"RefreshDen", bestDen, FALSE);
}

static void UnmapRegisters(Device* d)
{
    StopInterruptRing(d);
    if (d->regs) d->dxgk.DxgkCbUnmapMemory(d->dxgk.DeviceHandle, (PVOID)d->regs);
    d->regs = NULL;
}

// ---- the picture ----

static void ReleaseFrameBuffer(Device* d)
{
    if (d->fb) MmUnmapIoSpace(d->fb, d->fbBytes);
    d->fb = NULL;
    d->fbBytes = 0;
}

static void BlackOut(Device* d)
{
    if (!d->fb) return;
    for (ULONG y = 0; y < d->height; ++y) RtlZeroMemory(d->fb + (SIZE_T)y * d->pitch, (SIZE_T)d->width * 4);
}

static void CopyRect(Device* d, const UCHAR* src, LONG srcPitch, LONG left, LONG top, LONG right, LONG bottom)
{
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (LONG)d->width) right = (LONG)d->width;
    if (bottom > (LONG)d->height) bottom = (LONG)d->height;
    if (left >= right || top >= bottom) return;
    SIZE_T bytes = (SIZE_T)(right - left) * 4;
    for (LONG y = top; y < bottom; ++y)
        RtlCopyMemory(d->fb + (SIZE_T)y * d->pitch + (SIZE_T)left * 4, src + (SSIZE_T)y * srcPitch + (SIZE_T)left * 4, bytes);
}

// ---- device life cycle ----

// An idle desktop presents nothing, so "the start went fine" cannot depend on presents alone: 20 s after a start that
// is still running, this thread clears the marker.
static VOID TrustThread(PVOID context)
{
    Device* d = (Device*)context;
    LARGE_INTEGER wait;
    wait.QuadPart = -20LL * 10000000LL;
    if (KeWaitForSingleObject(&d->stopEvent, Executive, KernelMode, FALSE, &wait) == STATUS_TIMEOUT && d->started && !d->trusted) {
        d->trusted = TRUE;
        RegSet(L"Presents", (ULONG)d->presents, FALSE);
        RegSet(L"StartPending", 0, TRUE);
        Stage(8);                                 // trusted by time (7 = by presents)
        if (d->full && gBootStarted) BootSettle(TRUE, "full mode trusted");
    }
    RegSet(L"IrqCount", (ULONG)d->irqCount, FALSE);
    RegSet(L"IrqLastMessage", d->irqLastMessage, FALSE);
    RegSet(L"RegRefused", d->refused, FALSE);
    // While our interrupt ring runs (a development setting), report its counters every 2 s.
    wait.QuadPart = -2LL * 10000000LL;
    while (d->ihOn && KeWaitForSingleObject(&d->stopEvent, Executive, KernelMode, FALSE, &wait) == STATUS_TIMEOUT) {
        RegSet(L"IrqCount", (ULONG)d->irqCount, FALSE);
        RegSet(L"IhEntries", (ULONG)d->ihEntries, FALSE);
        RegSet(L"IhOverflows", d->ihOverflows, FALSE);
        RegSet(L"IhRptr", d->ihRptr, FALSE);
        RegSet(L"IhWptrReg", RegRead(d, rIH_RB_WPTR), FALSE);
        for (int i = 0; i < 8; ++i) {
            if (!d->ihStat[i].count) continue;
            WCHAR name[16] = L"IhSrc0";
            name[5] = (WCHAR)(L'0' + i);
            RegSet(name, d->ihStat[i].key, FALSE);
            name[2] = L'C'; name[3] = L'n'; name[4] = L't';
            RegSet(name, (ULONG)d->ihStat[i].count, FALSE);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void StopTrustThread(Device* d)
{
    if (!d->trustThread) return;
    KeSetEvent(&d->stopEvent, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(d->trustThread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(d->trustThread);
    d->trustThread = NULL;
}

static NTSTATUS APIENTRY DdiAddDevice(IN_CONST_PDEVICE_OBJECT pdo, OUT_PPVOID context)
{
    Tr tr("DdiAddDevice");
    Device* d = (Device*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(Device), P5_TAG);      // zeroed; Windows 10 2004 and later
    if (!d) return STATUS_NO_MEMORY;
    d->pdo = pdo;
    d->full = gFull;
    *context = d;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY DdiRemoveDevice(IN_CONST_PVOID context)
{
    Tr tr("DdiRemoveDevice");
    Device* d = (Device*)context;
    StopTrustThread(d);
    ReleaseFrameBuffer(d);
    ExFreePoolWithTag(d, P5_TAG);
    return STATUS_SUCCESS;
}

static NTSTATUS Fail(ULONG stage, NTSTATUS status)
{
    RegSet(L"FailStage", stage, FALSE);
    RegSet(L"LastStatus", (ULONG)status, FALSE);
    RegSet(L"StartPending", 0, TRUE);          // a start that failed cleanly is not a crash: try again next time
    if (gBootStarted) BootSettle(FALSE, "the full-mode device start failed");
    return status;
}

static NTSTATUS APIENTRY DdiStartDevice(IN_CONST_PVOID context, IN_PDXGK_START_INFO, IN_PDXGKRNL_INTERFACE dxgk, OUT_PULONG sources, OUT_PULONG children)
{
    Tr tr("DdiStartDevice");
    Device* d = (Device*)context;
    if (RegGet(L"StartPending", 0) != 0) {
        // The previous start never reached "trusted" (crash, hang, or the machine was switched off early).
        RegSet(L"Refused", RegGet(L"Refused", 0) + 1, TRUE);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    RegSet(L"StartPending", 1, TRUE);
    RegSet(L"Presents", 0, FALSE);
    Stage(1);
    if (d->full) {
        TraceOpen();
        LogF("start allowed, trace open (WDDM %s)", gWddm2 ? "2.0" : "1.3");
    }
    RtlCopyMemory(&d->dxgk, dxgk, sizeof d->dxgk);

    // Take the picture as the firmware (or the basic display driver before us) left it.
    NTSTATUS st = d->dxgk.DxgkCbAcquirePostDisplayOwnership(d->dxgk.DeviceHandle, &d->disp);
    RegSet(L"LastStatus", (ULONG)st, FALSE);
    if (!NT_SUCCESS(st)) return Fail(2, st);
    Stage(2);
    RegSet(L"Width", d->disp.Width, FALSE);
    RegSet(L"Height", d->disp.Height, FALSE);
    RegSet(L"Pitch", d->disp.Pitch, FALSE);
    RegSet(L"ColorFormat", (ULONG)d->disp.ColorFormat, FALSE);
    RegSet(L"PhysLow", d->disp.PhysicAddress.LowPart, FALSE);
    RegSet(L"PhysHigh", (ULONG)d->disp.PhysicAddress.HighPart, FALSE);
    RegSet(L"TargetId", d->disp.TargetId, FALSE);
    if (d->disp.Width < 640 || d->disp.Height < 480 || d->disp.Width > 8192 || d->disp.Height > 4320 || d->disp.Pitch < d->disp.Width * 4 ||
        d->disp.PhysicAddress.QuadPart == 0 || (d->disp.ColorFormat != D3DDDIFMT_A8R8G8B8 && d->disp.ColorFormat != D3DDDIFMT_X8R8G8B8))
        return Fail(3, STATUS_UNSUCCESSFUL);    // not the one kind of picture this driver knows
    d->ownsDisplay = TRUE;
    d->width = d->disp.Width;
    d->height = d->disp.Height;
    d->pitch = d->disp.Pitch;
    d->fbBytes = (SIZE_T)d->pitch * d->height;
    d->fb = (UCHAR*)MmMapIoSpace(d->disp.PhysicAddress, d->fbBytes, MmWriteCombined);
    if (!d->fb) return Fail(4, STATUS_INSUFFICIENT_RESOURCES);
    Stage(4);
    MapRegisters(d);
    MeasureRefresh(d);
    if (d->regs) QuiesceInterruptRings(d);
    if (d->regs && RegGet(L"IhEnable", 0) == 1) StartInterruptRing(d);
    RegSet(L"RegsMapped", d->regs != NULL, FALSE);
    *sources = 1;
    *children = 1;
    KeQuerySystemTime(&d->startTime);
    d->started = TRUE;
    d->trusted = FALSE;
    KeInitializeEvent(&d->stopEvent, NotificationEvent, FALSE);
    HANDLE thread = NULL;
    if (NT_SUCCESS(PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, TrustThread, d))) {
        if (!NT_SUCCESS(ObReferenceObjectByHandle(thread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID*)&d->trustThread, NULL))) d->trustThread = NULL;
        ZwClose(thread);
    }
    if (d->full) {
        if (!SetupVram(d)) {
            LogF("full mode needs the GPU's memory: start refused");
            LogFlush();
            TraceClose();
            d->started = FALSE;
            StopTrustThread(d);
            ReleaseVram(d);
            UnmapRegisters(d);
            return Fail(7, STATUS_DEVICE_CONFIGURATION_ERROR);
        }
        StartEngine(d);
        LogF("started full: %ux%u pitch %u%s", d->width, d->height, d->pitch, gBootStarted ? " (GPU started at boot by this driver)" : "");
    }
    RegSet(L"FullMode", d->full, FALSE);
    Stage(5);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY DdiStopDevice(IN_CONST_PVOID context)
{
    Tr tr("DdiStopDevice");
    Device* d = (Device*)context;
    d->started = FALSE;
    if (d->full) ShowFrameBuffer(d);
    StopEngine(d);
    StopTrustThread(d);
    if (d->full && gBootStarted) BootSettle(FALSE, "device stopped before full mode was trusted");
    if (d->full) ReleaseVram(d);
    UnmapRegisters(d);
    ReleaseFrameBuffer(d);
    RegSet(L"StartPending", 0, TRUE);
    Stage(90);
    if (d->full) { LogF("stopped"); LogFlush(); TraceClose(); }
    return STATUS_SUCCESS;
}

// A stop that hands the picture on (to the basic driver, or to a newer version of this one).
static NTSTATUS APIENTRY DdiStopDeviceAndReleasePostDisplayOwnership(IN_CONST_PVOID context, IN_CONST_D3DDDI_VIDEO_PRESENT_TARGET_ID, PDXGK_DISPLAY_INFORMATION info)
{
    Tr tr("DdiStopDeviceAndReleasePostDisplayOwnership");
    Device* d = (Device*)context;
    if (d->ownsDisplay) *info = d->disp;
    d->started = FALSE;
    if (d->full) ShowFrameBuffer(d);
    StopEngine(d);
    StopTrustThread(d);
    if (d->full && gBootStarted) BootSettle(FALSE, "device stopped before full mode was trusted");
    if (d->full) ReleaseVram(d);
    UnmapRegisters(d);
    ReleaseFrameBuffer(d);
    RegSet(L"StartPending", 0, TRUE);
    Stage(91);
    if (d->full) { LogF("stopped, display handed on"); LogFlush(); TraceClose(); }
    return d->ownsDisplay ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static NTSTATUS APIENTRY DdiDispatchIoRequest(IN_CONST_PVOID, IN_ULONG, IN_PVIDEO_REQUEST_PACKET) { Tr tr("DdiDispatchIoRequest"); return STATUS_NOT_SUPPORTED; }
static BOOLEAN APIENTRY DdiInterruptRoutine(IN_CONST_PVOID context, IN_ULONG message)
{
    Device* d = (Device*)context;
    InterlockedIncrement(&d->irqCount);
    d->irqLastMessage = message;
    if (d->ihOn) d->dxgk.DxgkCbQueueDpc(d->dxgk.DeviceHandle);
    return TRUE;
}
static VOID APIENTRY DdiDpcRoutine(IN_CONST_PVOID context) { DrainInterruptRing((Device*)context); }
static NTSTATUS APIENTRY DdiSetPowerState(IN_CONST_PVOID, IN_ULONG, IN_DEVICE_POWER_STATE, IN_POWER_ACTION) { Tr tr("DdiSetPowerState"); return STATUS_SUCCESS; }
static NTSTATUS APIENTRY DdiNotifyAcpiEvent(IN_CONST_PVOID, IN_DXGK_EVENT_TYPE, IN_ULONG, IN_PVOID, OUT_PULONG) { Tr tr("DdiNotifyAcpiEvent"); return STATUS_SUCCESS; }
static VOID APIENTRY DdiResetDevice(IN_CONST_PVOID) {}
static CHAR* gTrace;
static VOID APIENTRY DdiUnload(VOID)
{
    if (gTrace) ExFreePoolWithTag(gTrace, P5_TAG);
    gTrace = NULL;
}
static NTSTATUS APIENTRY DdiQueryInterface(IN_CONST_PVOID, IN_PQUERY_INTERFACE) { Tr tr("DdiQueryInterface"); return STATUS_NOT_SUPPORTED; }

// ---- the one monitor ----

static NTSTATUS APIENTRY DdiQueryChildRelations(IN_CONST_PVOID, _Inout_updates_bytes_(size) PDXGK_CHILD_DESCRIPTOR relations, _In_ ULONG size)
{
    Tr tr("DdiQueryChildRelations");
    if (size < 2 * sizeof(DXGK_CHILD_DESCRIPTOR)) return STATUS_BUFFER_TOO_SMALL;     // one child and the empty terminator
    relations[0].ChildDeviceType = TypeVideoOutput;
    relations[0].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;
    relations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_OTHER;
    relations[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    relations[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    relations[0].AcpiUid = 0;
    relations[0].ChildUid = 0;                  // our one monitor; the firmware's TargetId is only recorded (registry)
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY DdiQueryChildStatus(IN_CONST_PVOID, INOUT_PDXGK_CHILD_STATUS status, IN_BOOLEAN)
{
    Tr tr("DdiQueryChildStatus");
    switch (status->Type) {
    case StatusConnection:
        status->HotPlug.Connected = TRUE;
        return STATUS_SUCCESS;
    case StatusRotation:
        status->Rotation.Angle = 0;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

// No EDID: the mode comes from RecommendMonitorModes.
static NTSTATUS APIENTRY DdiQueryDeviceDescriptor(IN_CONST_PVOID, IN_ULONG, INOUT_PDXGK_DEVICE_DESCRIPTOR) { Tr tr("DdiQueryDeviceDescriptor"); return STATUS_MONITOR_NO_DESCRIPTOR; }

// ---- what the adapter can do ----

static NTSTATUS APIENTRY DdiQueryAdapterInfo(IN_CONST_HANDLE, IN_CONST_PDXGKARG_QUERYADAPTERINFO q)
{
    Tr tr("DdiQueryAdapterInfo");
    if (q->Type != DXGKQAITYPE_DRIVERCAPS) return STATUS_NOT_SUPPORTED;
    // Windows hands a display-only driver the Windows 8 size of this structure, which is smaller than the one in the
    // headers we build against now (0.3.0 refused it and failed after start): only what we fill in has to fit.
    const ULONG need = FIELD_OFFSET(DXGK_DRIVERCAPS, SupportSmoothRotation) + sizeof(BOOLEAN);
    if (q->OutputDataSize < need) return STATUS_BUFFER_TOO_SMALL;
    DXGK_DRIVERCAPS* caps = (DXGK_DRIVERCAPS*)q->pOutputData;
    RtlZeroMemory(caps, q->OutputDataSize < sizeof *caps ? q->OutputDataSize : sizeof *caps);
    caps->WDDMVersion = DXGKDDI_WDDMv1_2;
    caps->HighestAcceptableAddress.QuadPart = -1;
    caps->SupportNonVGA = TRUE;
    caps->SupportSmoothRotation = FALSE;
    return STATUS_SUCCESS;                      // no pointer caps: Windows draws the mouse pointer into what it presents
}

static NTSTATUS APIENTRY DdiSetPointerPosition(IN_CONST_HANDLE, IN_CONST_PDXGKARG_SETPOINTERPOSITION) { return STATUS_SUCCESS; }
static NTSTATUS APIENTRY DdiSetPointerShape(IN_CONST_HANDLE, IN_CONST_PDXGKARG_SETPOINTERSHAPE) { Tr tr("DdiSetPointerShape"); return STATUS_NOT_SUPPORTED; }

// ---- the one mode ----

static void FillSignal(Device* d, D3DKMDT_VIDEO_SIGNAL_INFO* s)
{
    s->VideoStandard = D3DKMDT_VSS_OTHER;
    s->TotalSize.cx = s->ActiveSize.cx = d->width;
    s->TotalSize.cy = s->ActiveSize.cy = d->height;
    s->VSyncFreq.Numerator = s->VSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    s->HSyncFreq.Numerator = s->HSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    s->PixelRate = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    if (d->full) {
        // "Not specified" is for display-only drivers. With it a full driver gets its modes asked for and then no
        // mode set at all (2026-11-20: Windows opened the adapter, enumerated, and left the screen where it was).
        // The measured refresh rate (MeasureRefresh; 60 Hz if unknown), with totals equal to the visible size, as viogpu3d
        // does for its fixed 60 Hz.
        ULONG num = d->refreshNum ? d->refreshNum : 60, den = d->refreshDen ? d->refreshDen : 1;
        s->VSyncFreq.Numerator = num;
        s->VSyncFreq.Denominator = den;
        s->HSyncFreq.Numerator = num * d->height;
        s->HSyncFreq.Denominator = den;
        s->PixelRate = (SIZE_T)((ULONGLONG)num * d->height * d->width / den);
    }
    s->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

static NTSTATUS AddSourceMode(Device* d, D3DKMDT_HVIDPN vidpn, const DXGK_VIDPN_INTERFACE* vi, D3DDDI_VIDEO_PRESENT_SOURCE_ID source)
{
    D3DKMDT_HVIDPNSOURCEMODESET set = NULL;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE* si = NULL;
    NTSTATUS st = vi->pfnCreateNewSourceModeSet(vidpn, source, &set, &si);
    if (!NT_SUCCESS(st)) return st;
    // The screen's one mode, as BGRA (what the desktop uses) and as RGBA. DXGI lists and matches display modes only in
    // formats the source mode set names, and games (and dgVoodoo, through Windows' own DXGI) ask for RGBA: without the
    // second entry FindClosestMatchingMode(R8G8B8A8) fails and they give up (SCP: Containment Breach, 2026-11-21).
    // Registry value RgbaMode=0 leaves it out. The display keeps reading BGRA (see DdiCommitVidPn's log line).
    static const D3DDDIFORMAT kFormats[2] = { D3DDDIFMT_A8R8G8B8, D3DDDIFMT_A8B8G8R8 };
    const ULONG nformats = RegGet(L"RgbaMode", 1) ? 2 : 1;
    for (ULONG k = 0; k < nformats && NT_SUCCESS(st); ++k) {
        D3DKMDT_VIDPN_SOURCE_MODE* mode = NULL;
        st = si->pfnCreateNewModeInfo(set, &mode);
        if (!NT_SUCCESS(st)) break;
        mode->Type = D3DKMDT_RMT_GRAPHICS;
        mode->Format.Graphics.PrimSurfSize.cx = mode->Format.Graphics.VisibleRegionSize.cx = d->width;
        mode->Format.Graphics.PrimSurfSize.cy = mode->Format.Graphics.VisibleRegionSize.cy = d->height;
        mode->Format.Graphics.Stride = d->pitch;
        mode->Format.Graphics.PixelFormat = kFormats[k];
        mode->Format.Graphics.ColorBasis = D3DKMDT_CB_SCRGB;
        mode->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
        st = si->pfnAddMode(set, mode);
        if (!NT_SUCCESS(st)) si->pfnReleaseModeInfo(set, mode);
    }
    if (NT_SUCCESS(st)) st = vi->pfnAssignSourceModeSet(vidpn, source, set);
    if (!NT_SUCCESS(st)) vi->pfnReleaseSourceModeSet(vidpn, set);
    return st;
}

static NTSTATUS AddTargetMode(Device* d, D3DKMDT_HVIDPN vidpn, const DXGK_VIDPN_INTERFACE* vi, D3DDDI_VIDEO_PRESENT_TARGET_ID target)
{
    D3DKMDT_HVIDPNTARGETMODESET set = NULL;
    const DXGK_VIDPNTARGETMODESET_INTERFACE* ti = NULL;
    NTSTATUS st = vi->pfnCreateNewTargetModeSet(vidpn, target, &set, &ti);
    if (!NT_SUCCESS(st)) return st;
    D3DKMDT_VIDPN_TARGET_MODE* mode = NULL;
    st = ti->pfnCreateNewModeInfo(set, &mode);
    if (NT_SUCCESS(st)) {
        FillSignal(d, &mode->VideoSignalInfo);
        mode->Preference = D3DKMDT_MP_PREFERRED;
        st = ti->pfnAddMode(set, mode);
        if (!NT_SUCCESS(st)) ti->pfnReleaseModeInfo(set, mode);
    }
    if (NT_SUCCESS(st)) st = vi->pfnAssignTargetModeSet(vidpn, target, set);
    if (!NT_SUCCESS(st)) vi->pfnReleaseTargetModeSet(vidpn, set);
    return st;
}

static NTSTATUS APIENTRY DdiIsSupportedVidPn(IN_CONST_HANDLE context, INOUT_PDXGKARG_ISSUPPORTEDVIDPN arg)
{
    Tr tr("DdiIsSupportedVidPn");
    Device* d = (Device*)context;
    arg->IsVidPnSupported = FALSE;
    if (arg->hDesiredVidPn == 0) {              // "nothing shown" is always possible
        arg->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }
    const DXGK_VIDPN_INTERFACE* vi = NULL;
    NTSTATUS st = d->dxgk.DxgkCbQueryVidPnInterface(arg->hDesiredVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vi);
    if (!NT_SUCCESS(st)) return st;
    D3DKMDT_HVIDPNTOPOLOGY topology = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE* ti = NULL;
    st = vi->pfnGetTopology(arg->hDesiredVidPn, &topology, &ti);
    if (!NT_SUCCESS(st)) return st;
    SIZE_T paths = 0;
    st = ti->pfnGetNumPaths(topology, &paths);
    if (!NT_SUCCESS(st)) return st;
    arg->IsVidPnSupported = paths <= 1;         // one source, one monitor
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY DdiRecommendFunctionalVidPn(IN_CONST_HANDLE, IN_CONST_PDXGKARG_RECOMMENDFUNCTIONALVIDPN_CONST)
{
    Tr tr("DdiRecommendFunctionalVidPn");
    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

static NTSTATUS APIENTRY DdiEnumVidPnCofuncModality(IN_CONST_HANDLE context, IN_CONST_PDXGKARG_ENUMVIDPNCOFUNCMODALITY_CONST arg)
{
    Tr tr("DdiEnumVidPnCofuncModality");
    Device* d = (Device*)context;
    const DXGK_VIDPN_INTERFACE* vi = NULL;
    NTSTATUS st = d->dxgk.DxgkCbQueryVidPnInterface(arg->hConstrainingVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vi);
    if (!NT_SUCCESS(st)) return st;
    D3DKMDT_HVIDPNTOPOLOGY topology = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE* ti = NULL;
    st = vi->pfnGetTopology(arg->hConstrainingVidPn, &topology, &ti);
    if (!NT_SUCCESS(st)) return st;

    const D3DKMDT_VIDPN_PRESENT_PATH* path = NULL;
    st = ti->pfnAcquireFirstPathInfo(topology, &path);
    while (st == STATUS_SUCCESS && path) {
        // Source: offer our mode unless one is pinned already or this source is the pivot of the enumeration.
        {
            D3DKMDT_HVIDPNSOURCEMODESET set = NULL;
            const DXGK_VIDPNSOURCEMODESET_INTERFACE* si = NULL;
            const D3DKMDT_VIDPN_SOURCE_MODE* pinned = NULL;
            NTSTATUS s2 = vi->pfnAcquireSourceModeSet(arg->hConstrainingVidPn, path->VidPnSourceId, &set, &si);
            if (NT_SUCCESS(s2)) {
                s2 = si->pfnAcquirePinnedModeInfo(set, &pinned);
                if (pinned) si->pfnReleaseModeInfo(set, pinned);
                vi->pfnReleaseSourceModeSet(arg->hConstrainingVidPn, set);
                BOOLEAN pivot = arg->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE && arg->EnumPivot.VidPnSourceId == path->VidPnSourceId;
                if (NT_SUCCESS(s2) && !pinned && !pivot) s2 = AddSourceMode(d, arg->hConstrainingVidPn, vi, path->VidPnSourceId);
            }
            if (!NT_SUCCESS(s2)) { ti->pfnReleasePathInfo(topology, path); return s2; }
        }
        // Target: the same.
        {
            D3DKMDT_HVIDPNTARGETMODESET set = NULL;
            const DXGK_VIDPNTARGETMODESET_INTERFACE* tmi = NULL;
            const D3DKMDT_VIDPN_TARGET_MODE* pinned = NULL;
            NTSTATUS s2 = vi->pfnAcquireTargetModeSet(arg->hConstrainingVidPn, path->VidPnTargetId, &set, &tmi);
            if (NT_SUCCESS(s2)) {
                s2 = tmi->pfnAcquirePinnedModeInfo(set, &pinned);
                if (pinned) tmi->pfnReleaseModeInfo(set, pinned);
                vi->pfnReleaseTargetModeSet(arg->hConstrainingVidPn, set);
                BOOLEAN pivot = arg->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET && arg->EnumPivot.VidPnTargetId == path->VidPnTargetId;
                if (NT_SUCCESS(s2) && !pinned && !pivot) s2 = AddTargetMode(d, arg->hConstrainingVidPn, vi, path->VidPnTargetId);
            }
            if (!NT_SUCCESS(s2)) { ti->pfnReleasePathInfo(topology, path); return s2; }
        }
        // Path: no scaling, no rotation, wherever Windows has not decided yet.
        D3DKMDT_VIDPN_PRESENT_PATH update = *path;
        BOOLEAN changed = FALSE;
        if (path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED &&
            !(arg->EnumPivotType == D3DKMDT_EPT_SCALING && arg->EnumPivot.VidPnSourceId == path->VidPnSourceId && arg->EnumPivot.VidPnTargetId == path->VidPnTargetId)) {
            RtlZeroMemory(&update.ContentTransformation.ScalingSupport, sizeof update.ContentTransformation.ScalingSupport);
            update.ContentTransformation.ScalingSupport.Identity = 1;
            changed = TRUE;
        }
        if (path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED &&
            !(arg->EnumPivotType == D3DKMDT_EPT_ROTATION && arg->EnumPivot.VidPnSourceId == path->VidPnSourceId && arg->EnumPivot.VidPnTargetId == path->VidPnTargetId)) {
            RtlZeroMemory(&update.ContentTransformation.RotationSupport, sizeof update.ContentTransformation.RotationSupport);
            update.ContentTransformation.RotationSupport.Identity = 1;
            changed = TRUE;
        }
        if (changed) {
            NTSTATUS s2 = ti->pfnUpdatePathSupportInfo(topology, &update);
            if (!NT_SUCCESS(s2)) { ti->pfnReleasePathInfo(topology, path); return s2; }
        }
        const D3DKMDT_VIDPN_PRESENT_PATH* next = NULL;
        st = ti->pfnAcquireNextPathInfo(topology, path, &next);
        ti->pfnReleasePathInfo(topology, path);
        path = next;
    }
    if (st == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET || st == STATUS_GRAPHICS_DATASET_IS_EMPTY) st = STATUS_SUCCESS;
    return st;
}

static NTSTATUS APIENTRY DdiSetVidPnSourceVisibility(IN_CONST_HANDLE context, IN_CONST_PDXGKARG_SETVIDPNSOURCEVISIBILITY arg)
{
    Tr tr("DdiSetVidPnSourceVisibility");
    Device* d = (Device*)context;
    if (!arg->Visible) BlackOut(d);
    return STATUS_SUCCESS;
}

// The mode never changes, so there is nothing to program.
static NTSTATUS APIENTRY DdiCommitVidPn(IN_CONST_HANDLE context, IN_CONST_PDXGKARG_COMMITVIDPN_CONST arg)
{
    Tr tr("DdiCommitVidPn");
    Stage(6);
    Device* d = (Device*)context;
    const DXGK_VIDPN_INTERFACE* vi = NULL;
    if (arg->hFunctionalVidPn && NT_SUCCESS(d->dxgk.DxgkCbQueryVidPnInterface(arg->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vi))) {
        D3DKMDT_HVIDPNSOURCEMODESET set = NULL;
        const DXGK_VIDPNSOURCEMODESET_INTERFACE* si = NULL;
        if (NT_SUCCESS(vi->pfnAcquireSourceModeSet(arg->hFunctionalVidPn, arg->AffectedVidPnSourceId, &set, &si))) {
            const D3DKMDT_VIDPN_SOURCE_MODE* pinned = NULL;
            if (NT_SUCCESS(si->pfnAcquirePinnedModeInfo(set, &pinned)) && pinned) {
                LogF("commit: source %u %ux%u format %u", arg->AffectedVidPnSourceId, pinned->Format.Graphics.PrimSurfSize.cx,
                     pinned->Format.Graphics.PrimSurfSize.cy, (ULONG)pinned->Format.Graphics.PixelFormat);
                si->pfnReleaseModeInfo(set, pinned);
            }
            vi->pfnReleaseSourceModeSet(arg->hFunctionalVidPn, set);
        }
    }
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY DdiUpdateActiveVidPnPresentPath(IN_CONST_HANDLE, IN_CONST_PDXGKARG_UPDATEACTIVEVIDPNPRESENTPATH_CONST) { Tr tr("DdiUpdateActiveVidPnPresentPath"); return STATUS_SUCCESS; }

static NTSTATUS APIENTRY DdiRecommendMonitorModes(IN_CONST_HANDLE context, IN_CONST_PDXGKARG_RECOMMENDMONITORMODES_CONST arg)
{
    Tr tr("DdiRecommendMonitorModes");
    Device* d = (Device*)context;
    D3DKMDT_MONITOR_SOURCE_MODE* mode = NULL;
    NTSTATUS st = arg->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(arg->hMonitorSourceModeSet, &mode);
    if (!NT_SUCCESS(st)) return st;
    FillSignal(d, &mode->VideoSignalInfo);
    mode->Origin = D3DKMDT_MCO_DRIVER;
    mode->Preference = D3DKMDT_MP_PREFERRED;
    mode->ColorBasis = D3DKMDT_CB_SRGB;
    mode->ColorCoeffDynamicRanges.FirstChannel = 8;
    mode->ColorCoeffDynamicRanges.SecondChannel = 8;
    mode->ColorCoeffDynamicRanges.ThirdChannel = 8;
    mode->ColorCoeffDynamicRanges.FourthChannel = 8;
    st = arg->pMonitorSourceModeSetInterface->pfnAddMode(arg->hMonitorSourceModeSet, mode);
    if (!NT_SUCCESS(st)) {
        arg->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(arg->hMonitorSourceModeSet, mode);
        if (st == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET) st = STATUS_SUCCESS;
    }
    return st;
}

static NTSTATUS APIENTRY DdiQueryVidPnHWCapability(IN_CONST_HANDLE, INOUT_PDXGKARG_QUERYVIDPNHWCAPABILITY arg)
{
    Tr tr("DdiQueryVidPnHWCapability");
    arg->VidPnHWCaps.DriverRotation = FALSE;
    arg->VidPnHWCaps.DriverScaling = FALSE;
    arg->VidPnHWCaps.DriverCloning = FALSE;
    arg->VidPnHWCaps.DriverColorConvert = FALSE;
    arg->VidPnHWCaps.DriverLinkedAdapaterOutput = FALSE;
    arg->VidPnHWCaps.DriverRemoteDisplay = FALSE;
    return STATUS_SUCCESS;
}

// ---- showing what Windows drew ----

static NTSTATUS APIENTRY DdiPresentDisplayOnly(IN_CONST_HANDLE context, IN_CONST_PDXGKARG_PRESENT_DISPLAYONLY arg)
{
    Tr tr("DdiPresentDisplayOnly");
    Device* d = (Device*)context;
    if (!d->started || !d->fb) return STATUS_SUCCESS;
    if (arg->BytesPerPixel != 4 || arg->Flags.Rotate) return STATUS_NOT_SUPPORTED;
    const UCHAR* src = (const UCHAR*)arg->pSource;
    // The source holds the new content of moved areas too, at their destination, so a move is a copy like a dirty rectangle.
    for (ULONG i = 0; i < arg->NumMoves; ++i) {
        const RECT& r = arg->pMoves[i].DestRect;
        CopyRect(d, src, arg->Pitch, r.left, r.top, r.right, r.bottom);
    }
    for (ULONG i = 0; i < arg->NumDirtyRects; ++i) {
        const RECT& r = arg->pDirtyRect[i];
        CopyRect(d, src, arg->Pitch, r.left, r.top, r.right, r.bottom);
    }
    LONG n = InterlockedIncrement(&d->presents);
    LARGE_INTEGER nowTime;
    KeQuerySystemTime(&nowTime);
    if (!d->trusted && ((ULONG)n >= kPresentsToTrust || nowTime.QuadPart - d->startTime.QuadPart > 15 * 10000000LL) && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        d->trusted = TRUE;
        RegSet(L"Presents", (ULONG)n, FALSE);
        RegSet(L"StartPending", 0, TRUE);
        Stage(7);
    }
    return STATUS_SUCCESS;
}

// The blue screen: Windows asks for the frame buffer back with interrupts off.
static NTSTATUS APIENTRY DdiSystemDisplayEnable(PVOID context, D3DDDI_VIDEO_PRESENT_TARGET_ID, PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS, UINT* width, UINT* height,
                                                D3DDDIFORMAT* format)
{
    Device* d = (Device*)context;
    if (!d->fb) return STATUS_UNSUCCESSFUL;
    if (d->full && d->flipOk) {                 // the stop screen is drawn into the frame buffer: make the display read it,
        ScanRec* keep = d->scanRec;             // whoever has the screen
        d->scanRec = NULL;
        FlipTo(d, d->fbScan);
        d->scanRec = keep;
    }
    *width = d->width;
    *height = d->height;
    *format = D3DDDIFMT_A8R8G8B8;
    return STATUS_SUCCESS;
}

static VOID APIENTRY DdiSystemDisplayWrite(PVOID context, PVOID source, UINT sourceWidth, UINT sourceHeight, UINT sourceStride, UINT x, UINT y)
{
    Device* d = (Device*)context;
    if (!d->fb || x >= d->width || y >= d->height) return;
    if (sourceWidth > d->width - x) sourceWidth = d->width - x;
    if (sourceHeight > d->height - y) sourceHeight = d->height - y;
    for (UINT row = 0; row < sourceHeight; ++row)
        RtlCopyMemory(d->fb + (SIZE_T)(y + row) * d->pitch + (SIZE_T)x * 4, (const UCHAR*)source + (SIZE_T)row * sourceStride, (SIZE_T)sourceWidth * 4);
}

// =====================================================================================================================
// Full-driver mode (stage W3)
// =====================================================================================================================

// ---- a text log, readable with "reg query", and a trace file that survives a crash ----
// The trace file (C:\ps5\gpu\ps5wddm-trace.log) is 256 KiB of spaces written once; every line then goes straight to
// the disk (no cache, 4 KiB blocks rewritten in place, so no file-size update is pending either). Whoever logs waits
// for the write: up to 1 s at low interrupt level, 50 ms spinning at dispatch level. After a crash the last line is the
// last thing this driver did. A dump is no help here: this console has never written one.
static CHAR gLog[6000];
static ULONG gLogLen;
static volatile LONG gLogDirty;
static KSPIN_LOCK gLogLock;
static const ULONG kTraceBytes = 256 * 1024, kTraceBlock = 4096;
static volatile ULONG gTraceLen, gTraceWritten;
static HANDLE gTraceFile;
static PETHREAD gTraceThread;
static KEVENT gTraceWake, gTraceDone;
static volatile LONG gTraceStop, gTraceEvents;
static ULONGLONG gTraceT0;
static const LONG kTraceAll = 1500;      // this many entry-point calls are traced in full, after that the first three of each

static VOID TraceThread(PVOID)
{
    for (;;) {
        KeWaitForSingleObject(&gTraceWake, Executive, KernelMode, FALSE, NULL);
        for (;;) {
            ULONG len = gTraceLen, done = gTraceWritten;
            if (len <= done) break;
            ULONG from = done & ~(kTraceBlock - 1), to = (len + kTraceBlock - 1) & ~(kTraceBlock - 1);
            LARGE_INTEGER off;
            off.QuadPart = from;
            IO_STATUS_BLOCK io;
            ZwWriteFile(gTraceFile, NULL, NULL, NULL, &io, gTrace + from, to - from, &off, NULL);
            gTraceWritten = len;
            KeSetEvent(&gTraceDone, IO_NO_INCREMENT, FALSE);
        }
        if (gTraceStop) break;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void TraceSync(ULONG target, KIRQL irql)
{
    if (!gTraceThread || PsGetCurrentThread() == gTraceThread) return;
    KeSetEvent(&gTraceWake, IO_NO_INCREMENT, FALSE);
    if (irql <= APC_LEVEL) {
        LARGE_INTEGER t;
        t.QuadPart = -100000;                                    // 10 ms
        for (int i = 0; i < 100 && gTraceWritten < target; ++i) {
            KeClearEvent(&gTraceDone);
            if (gTraceWritten >= target) break;
            KeWaitForSingleObject(&gTraceDone, Executive, KernelMode, FALSE, &t);
        }
    } else if (irql == DISPATCH_LEVEL) {
        for (int i = 0; i < 1000 && gTraceWritten < target; ++i) KeStallExecutionProcessor(50);
    }
}

static void LogLine(const CHAR* line, BOOLEAN toRegistry)
{
    SIZE_T n = 0;
    RtlStringCbLengthA(line, 200, &n);
    KIRQL now = KeGetCurrentIrql(), irql;
    if (now > DISPATCH_LEVEL) return;
    CHAR head[24];
    SIZE_T hn = 0;
    if (!NT_SUCCESS(RtlStringCbPrintfA(head, sizeof head, "%7u %u ", (ULONG)((KeQueryInterruptTime() - gTraceT0) / 10000), (ULONG)now))) head[0] = 0;
    RtlStringCbLengthA(head, sizeof head, &hn);
    ULONG target = 0;
    KeAcquireSpinLock(&gLogLock, &irql);
    if (toRegistry && gLogLen + n + 2 < sizeof gLog) {
        RtlCopyMemory(gLog + gLogLen, line, n);
        gLogLen += (ULONG)n;
        gLog[gLogLen++] = '|';
        gLog[gLogLen] = 0;
        gLogDirty = 1;
    }
    if (gTrace && gTraceLen + hn + n + 2 < kTraceBytes) {
        ULONG at = gTraceLen;
        RtlCopyMemory(gTrace + at, head, hn);
        RtlCopyMemory(gTrace + at + hn, line, n);
        gTrace[at + hn + n] = '\r';
        gTrace[at + hn + n + 1] = '\n';
        gTraceLen = target = at + (ULONG)(hn + n) + 2;
    }
    KeReleaseSpinLock(&gLogLock, irql);
    if (target) TraceSync(target, now);
}

static void LogF(const char* fmt, ...)
{
    CHAR line[200];
    va_list ap;
    va_start(ap, fmt);
    if (!NT_SUCCESS(RtlStringCbVPrintfA(line, sizeof line, fmt, ap))) line[sizeof line - 1] = 0;
    va_end(ap);
    LogLine(line, TRUE);
}

static void TraceF(const char* fmt, ...)                         // trace file only
{
    CHAR line[200];
    va_list ap;
    va_start(ap, fmt);
    if (!NT_SUCCESS(RtlStringCbVPrintfA(line, sizeof line, fmt, ap))) line[sizeof line - 1] = 0;
    va_end(ap);
    LogLine(line, FALSE);
}

static void LogFlush()
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !InterlockedExchange(&gLogDirty, 0)) return;
    static WCHAR wide[sizeof gLog];
    ULONG n;
    KIRQL irql;
    KeAcquireSpinLock(&gLogLock, &irql);
    for (n = 0; n < gLogLen; ++n) wide[n] = (WCHAR)(UCHAR)gLog[n];
    wide[n] = 0;
    KeReleaseSpinLock(&gLogLock, irql);
    HANDLE key;
    if (!NT_SUCCESS(RegOpen(&key))) return;
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"Log");
    ZwSetValueKey(key, &name, 0, REG_SZ, wide, (n + 1) * sizeof(WCHAR));
    ZwClose(key);
}

// Called from DdiStartDevice once the start is allowed (never earlier: a start refused after a crash must leave the
// crashed run's trace as it is).
static void TraceOpen()
{
    if (gTraceThread) return;
    if (!gTrace) gTrace = (CHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, kTraceBytes, P5_TAG);
    if (!gTrace) return;
    RtlFillMemory(gTrace, kTraceBytes, ' ');
    gTraceLen = gTraceWritten = 0;
    gTraceStop = 0;
    gTraceEvents = 0;
    gTraceT0 = KeQueryInterruptTime();
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\??\\C:\\ps5\\gpu\\ps5wddm-trace.log");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    IO_STATUS_BLOCK io;
    NTSTATUS st = ZwCreateFile(&gTraceFile, GENERIC_WRITE | SYNCHRONIZE, &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OVERWRITE_IF,
                               FILE_SYNCHRONOUS_IO_NONALERT | FILE_NO_INTERMEDIATE_BUFFERING | FILE_WRITE_THROUGH | FILE_NON_DIRECTORY_FILE, NULL, 0);
    RegSet(L"TraceOpen", (ULONG)st, FALSE);
    if (!NT_SUCCESS(st)) { gTraceFile = NULL; return; }
    LARGE_INTEGER off;
    off.QuadPart = 0;
    st = ZwWriteFile(gTraceFile, NULL, NULL, NULL, &io, gTrace, kTraceBytes, &off, NULL);
    ZwFlushBuffersFile(gTraceFile, &io);
    RegSet(L"TraceFill", (ULONG)st, FALSE);
    KeInitializeEvent(&gTraceWake, SynchronizationEvent, FALSE);
    KeInitializeEvent(&gTraceDone, NotificationEvent, FALSE);
    HANDLE thread = NULL;
    if (NT_SUCCESS(PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, TraceThread, NULL))) {
        if (!NT_SUCCESS(ObReferenceObjectByHandle(thread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID*)&gTraceThread, NULL))) gTraceThread = NULL;
        ZwClose(thread);
    }
    if (!gTraceThread) { ZwClose(gTraceFile); gTraceFile = NULL; }
}

static void TraceClose()
{
    if (!gTraceThread) return;
    InterlockedExchange(&gTraceStop, 1);
    KeSetEvent(&gTraceWake, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(gTraceThread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(gTraceThread);
    gTraceThread = NULL;
    IO_STATUS_BLOCK io;
    ZwFlushBuffersFile(gTraceFile, &io);
    ZwClose(gTraceFile);
    gTraceFile = NULL;
}

static BOOLEAN FirstFew(const char* name)
{
    static struct { const char* name; LONG n; } seen[128];
    for (int i = 0; i < 128; ++i) {
        if (seen[i].name == name || !seen[i].name) {
            seen[i].name = name;
            return InterlockedIncrement(&seen[i].n) <= 3;
        }
    }
    return FALSE;
}

Tr::Tr(const char* n) : name(n), on(FALSE)
{
    if (!gFull || KeGetCurrentIrql() > DISPATCH_LEVEL) return;
    BOOLEAN few = FirstFew(n);
    on = few || InterlockedIncrement(&gTraceEvents) <= kTraceAll;
    if (on) TraceF(">%s", name);
}
Tr::~Tr()
{
    if (on) TraceF("<%s", name);
}

// ---- allocations ----
// What travels in an allocation's private data, from whoever creates it (us for the standard ones, later our Direct3D
// user-mode driver) to DxgkDdiCreateAllocation.
#define P5A_MAGIC 0x41355350u            // "PS5A"
enum : ULONG { P5A_PRIMARY = 1, P5A_SHADOW = 2, P5A_STAGING = 3, P5A_USER = 4, P5A_GDI = 5 };
struct P5AllocData {
    ULONG magic, kind, width, height, pitch, format;
    ULONGLONG bytes;
    ULONGLONG gpuPhys;                   // reserved for the user-mode driver
};
struct Alloc {
    P5AllocData info;
    UCHAR* va;                           // aperture segment: our mapping of its pages while Windows has it there
    PMDL mdl;
    SIZE_T pages;
    ULONGLONG apAddr;                    // ... and where in the aperture segment it sits
};
static ULONG KindOf(const Alloc* a) { return a->info.kind & 0xFF; }
static ULONG GdiTypeOf(const Alloc* a) { return a->info.kind >> 8; }
static ULONG BppOf(ULONG format)
{
    switch (format) {
    case 28: case 41: case 50: return 1;                 // D3DDDIFMT_A8, P8, L8
    case 23: case 24: case 25: return 2;                 // R5G6B5, X1R5G5B5, A1R5G5B5
    default: return 4;
    }
}
// The aperture segment (GDI's CPU-visible surfaces): system memory, cache-coherent. Its addresses are ours to choose.
static const ULONGLONG kApBase = 0x1000000000ull, kApBytes = 0x10000000ull;
static UCHAR* ApPtr(Alloc* a, ULONGLONG addr, ULONGLONG bytes)
{
    if (!a || !a->va || addr < a->apAddr) return NULL;
    ULONGLONG off = addr - a->apAddr, have = (ULONGLONG)a->pages * PAGE_SIZE;
    if (bytes > have || off > have - bytes) return NULL;
    return a->va + off;
}
static void UnmapAp(Alloc* a)
{
    if (!a || !a->mdl) return;
    if (a->va) MmUnmapLockedPages(a->va, a->mdl);
    IoFreeMdl(a->mdl);
    a->va = NULL;
    a->mdl = NULL;
    a->pages = 0;
}
struct Resource { ULONG magic; };

// ---- the segment: a range of the GPU's own memory that Windows places every allocation in ----
static UCHAR* SegPtr(Device* d, ULONGLONG addr, ULONGLONG bytes)
{
    if (!d->segVa || addr < d->segPhys || bytes > d->segBytes || addr - d->segPhys > d->segBytes - bytes) return NULL;
    return d->segVa + (SIZE_T)(addr - d->segPhys);
}
static UCHAR* AnyPtr(Device* d, ULONG seg, ULONGLONG addr, Alloc* a, ULONGLONG bytes)
{
    return seg == 2 ? ApPtr(a, addr, bytes) : SegPtr(d, addr, bytes);
}

// Reading write-combined memory with ordinary loads is very slow (2026-11-20: 180 ms for one 2560x1440 picture);
// the streaming load (MOVNTDQA) is what such memory is meant to be read with.
typedef long long P5V __attribute__((__vector_size__(16)));
__attribute__((target("sse4.1"))) static void CopyWc(UCHAR* dst, const UCHAR* src, SIZE_T n)
{
    while (n && ((ULONG_PTR)src & 15)) { *dst++ = *src++; --n; }
    while (n >= 16) {
        P5V v = __builtin_nontemporal_load((const P5V*)src);
        __builtin_memcpy(dst, &v, 16);
        src += 16; dst += 16; n -= 16;
    }
    while (n) { *dst++ = *src++; --n; }
}

// ---- the display: which picture it reads (same registers and order as libps5gpu's p5_scanout_flip) ----
struct Pipe { ULONG addrLo, addrHi, inUseLo, inUseHi, vpDim, vpStart, cntl; };
static const Pipe kPipes[4] = {
    {0x3aca, 0x3acb, 0x3ae5, 0x3ae6, 0x3aaa, 0x3aa9, 0x3ab3},
    {0x3ba6, 0x3ba7, 0x3bc1, 0x3bc2, 0x3b86, 0x3b85, 0x3b8f},
    {0x3c82, 0x3c83, 0x3c9d, 0x3c9e, 0x3c62, 0x3c61, 0x3c6b},
    {0x3d5e, 0x3d5f, 0x3d79, 0x3d7a, 0x3d3e, 0x3d3d, 0x3d47},
};
static const ULONG rOtgUpdateLock = 0x504b, rSurfacePitch = 0x3ac7, kUpdateLockStatus = 0x100, kHubpBlankEn = 0x1;

// libps5gpu's record of who has the screen (lib/ps5drm.cpp, ScanRec at area + 0x14100).
#define P5_SCANREC_MAGIC 0x4F533550u     // "P5SO"
struct ScanRec {
    ULONG magic, flipPid;
    ULONGLONG bootStamp, desktopPhys, flipPhys;
};
static ULONGLONG BootStamp()
{
    LARGE_INTEGER now;
    KeQuerySystemTime(&now);
    return ((ULONGLONG)now.QuadPart - KeQueryInterruptTime() + 80000000ull) / 160000000ull + 1;
}

// Which pipes show a picture and from where, and whether it has the size and pitch we know: only then are flips
// possible. Called at the start, when what the display reads is the frame buffer we were handed.
static void LookAtDisplay(Device* d)
{
    ULONG pipes = 0, width = 0, height = 0;
    ULONGLONG phys = 0;
    for (ULONG n = 0; n < 4; ++n) {
        const Pipe& p = kPipes[n];
        ULONG dim = RegRead(d, p.vpDim), start = RegRead(d, p.vpStart), cntl = RegRead(d, p.cntl);
        if ((cntl & kHubpBlankEn) || !(dim & 0x3FFF) || !((dim >> 16) & 0x3FFF)) continue;
        ULONGLONG addr = ((ULONGLONG)(RegRead(d, p.addrHi) & 0xFFFF) << 32) | RegRead(d, p.addrLo);
        if (!pipes) phys = addr;
        else if (addr != phys) { pipes = 0; break; }
        pipes |= 1u << n;
        if ((start & 0x3FFF) + (dim & 0x3FFF) > width) width = (start & 0x3FFF) + (dim & 0x3FFF);
        if (((dim >> 16) & 0x3FFF) > height) height = (dim >> 16) & 0x3FFF;
    }
    ULONG pitch = (RegRead(d, rSurfacePitch) & 0x3FFF) + 1;
    d->pipes = pipes;
    d->fbScan = pipes ? phys : 0;                            // what the display reads now is the frame buffer we were handed
    d->flipOk = pipes != 0 && width == d->width && height == d->height && pitch * 4 == d->pitch;
    LogF("display: pipes %X %ux%u pitch %u at %I64X -> flips %s", pipes, width, height, pitch, phys, d->flipOk ? "possible" : "NOT possible (copying)");
}

// The flip. Register writes only, so it may run at any interrupt level. A program that has taken the screen through
// libps5gpu keeps it: we only note where the desktop is now, and its release (or the watchdog) brings that back.
static void FlipTo(Device* d, ULONGLONG phys)
{
    if (!d->flipOk) return;
    ScanRec* r = d->scanRec;
    if (r && r->magic == P5_SCANREC_MAGIC) {
        ULONGLONG stamp = BootStamp();
        if (r->bootStamp + 1 >= stamp && r->bootStamp <= stamp + 1) {
            r->desktopPhys = phys;
            if (r->flipPid) { d->flipsHeld++; return; }
        }
    }
    RegWrite(d, rOtgUpdateLock, 1);
    for (int i = 0; i < 64; ++i)
        if (RegRead(d, rOtgUpdateLock) & kUpdateLockStatus) break;
    for (ULONG n = 0; n < 4; ++n) {
        if (!(d->pipes & (1u << n))) continue;
        RegWrite(d, kPipes[n].addrHi, (ULONG)(phys >> 32));
        RegWrite(d, kPipes[n].addrLo, (ULONG)phys);
    }
    RegWrite(d, rOtgUpdateLock, 0);
    d->shownPhys = phys;
    d->flipsDone++;
}

// Back to the frame buffer we were handed (stops, "not visible", a destroyed primary). Low interrupt level only:
// waits, briefly, until the display has really let go of the old picture.
static void ShowFrameBuffer(Device* d)
{
    if (!d->flipOk) return;
    FlipTo(d, d->fbScan);
    if (KeGetCurrentIrql() > APC_LEVEL) return;
    for (int i = 0; i < 100; ++i) {
        BOOLEAN taken = TRUE;
        for (ULONG n = 0; n < 4; ++n) {
            if (!(d->pipes & (1u << n))) continue;
            ULONGLONG inUse = ((ULONGLONG)(RegRead(d, kPipes[n].inUseHi) & 0xFFFF) << 32) | RegRead(d, kPipes[n].inUseLo);
            if (inUse != d->shownPhys) taken = FALSE;
        }
        if (taken) break;
        KeStallExecutionProcessor(500);
    }
}

// ---- work handed from SubmitCommand to the engine thread ----
enum : ULONG { OP_NOP = 0, OP_BLT = 1, OP_FILL = 2, OP_GDI = 3, OP_PAGE_IN = 10, OP_PAGE_OUT = 11, OP_PAGE_MOVE = 12, OP_PAGE_FILL = 13, OP_AP_MAP = 14, OP_AP_UNMAP = 15, OP_PREEMPT = 100 };
#define OP_MAGIC 0x3550504Fu             // "OPP5": private data we filled in ourselves (anything else is treated as empty)
struct Op {                              // lives in the DMA buffer's private data
    ULONG kind, color, nrects, magic;
    Alloc* src;
    Alloc* dst;
    ULONGLONG srcAddr, dstAddr;          // where they are in the segment when this runs (0: nowhere)
    ULONG srcSeg, dstSeg;                // ... in which segment (1 GPU memory, 2 aperture; 0: unknown, taken as 1)
    RECT srcRect, dstRect;
    RECT rects[16];
    PMDL mdl;                            // paging: the system-memory side as Windows gave it (only used at once, in
    ULONG mdlOffset;                     // DxgkDdiBuildPagingBuffer itself), first page in it
    ULONG ticket;                        // ... and for the engine thread: our own copy of its page numbers (TakeTicket)
    ULONGLONG bytes;                     // paging: how much
};
struct Work { ULONG fence, last; Op op; };
static const ULONG kQueue = 2048;

// Page-number copies wait here for the engine thread. An operation only carries the ticket number, so a buffer that
// Windows submits twice (or never) cannot make us free a copy twice: the second taker finds nothing.
static ULONG PutTicket(Device* d, PFN_NUMBER* pfns)
{
    KIRQL irql;
    ULONG id = 0;
    KeAcquireSpinLock(&d->queueLock, &irql);
    for (ULONG i = 0; i < kTickets; ++i) {
        if (d->tickets[i].id) continue;
        if (++d->nextTicket == 0) ++d->nextTicket;
        id = d->tickets[i].id = d->nextTicket;
        d->tickets[i].pfns = pfns;
        break;
    }
    KeReleaseSpinLock(&d->queueLock, irql);
    return id;
}
static PFN_NUMBER* TakeTicketLocked(Device* d, ULONG id)
{
    if (!id) return NULL;
    for (ULONG i = 0; i < kTickets; ++i) {
        if (d->tickets[i].id != id) continue;
        PFN_NUMBER* pfns = d->tickets[i].pfns;
        d->tickets[i].id = 0;
        d->tickets[i].pfns = NULL;
        return pfns;
    }
    return NULL;
}
static PFN_NUMBER* TakeTicket(Device* d, ULONG id)
{
    KIRQL irql;
    KeAcquireSpinLock(&d->queueLock, &irql);
    PFN_NUMBER* pfns = TakeTicketLocked(d, id);
    KeReleaseSpinLock(&d->queueLock, irql);
    return pfns;
}

static void WakeEngine(Device* d) { KeSetEvent(&d->engineWake, IO_NO_INCREMENT, FALSE); }

// The operations of one submission (n may be 0), in order. The last entry carries the completion; with nothing valid
// to do an empty entry of kind `emptyKind` does.
static void EnqueueOps(Device* d, ULONG fence, const Op* ops, ULONG n, ULONG emptyKind)
{
    KIRQL irql;
    KeAcquireSpinLock(&d->queueLock, &irql);
    Work* lastPut = NULL;
    for (ULONG i = 0; i <= n; ++i) {
        const Op* op = i < n ? &ops[i] : NULL;
        if (op && op->magic != OP_MAGIC) continue;
        if (!op && lastPut) break;                               // the extra, empty entry is only for "nothing put"
        ULONG next = (d->queueHead + 1) % kQueue;
        if (next == d->queueTail) { d->dropped++; break; }
        Work* w = &d->queue[d->queueHead];
        w->fence = fence;
        w->last = 0;
        if (op) w->op = *op;
        else { RtlZeroMemory(&w->op, sizeof w->op); w->op.kind = emptyKind; }
        d->queueHead = next;
        lastPut = w;
    }
    if (lastPut) lastPut->last = 1;
    KeReleaseSpinLock(&d->queueLock, irql);
    WakeEngine(d);
}

// ---- telling Windows: "buffer N is done", "preempted", "vertical sync" ----
struct NotifyCtx { Device* d; DXGKARGCB_NOTIFY_INTERRUPT_DATA* data; };
static BOOLEAN NTAPI NotifySync(PVOID context)
{
    NotifyCtx* n = (NotifyCtx*)context;
    n->d->dxgk.DxgkCbNotifyInterrupt(n->d->dxgk.DeviceHandle, n->data);
    InterlockedExchange(&n->d->notifyPending, 1);
    n->d->dxgk.DxgkCbQueueDpc(n->d->dxgk.DeviceHandle);
    return TRUE;
}
static void Notify(Device* d, DXGKARGCB_NOTIFY_INTERRUPT_DATA* data)
{
    static volatile LONG count[3];
    int kind = data->InterruptType == DXGK_INTERRUPT_CRTC_VSYNC ? 0 : data->InterruptType == DXGK_INTERRUPT_DMA_COMPLETED ? 1 : 2;
    BOOLEAN say = InterlockedIncrement(&count[kind]) <= 6;
    if (say) TraceF("notify %s fence %u ...", kind == 0 ? "vsync" : kind == 1 ? "completed" : "preempted", kind == 1 ? data->DmaCompleted.SubmissionFenceId : kind == 2 ? data->DmaPreempted.PreemptionFenceId : 0);
    NotifyCtx n = { d, data };
    BOOLEAN ret = FALSE;
    NTSTATUS st = d->dxgk.DxgkCbSynchronizeExecution(d->dxgk.DeviceHandle, NotifySync, &n, 0, &ret);
    if (say) TraceF("... notified (status %08X, ran %u)", (ULONG)st, (ULONG)ret);
}

// ---- CPU copies (until the GPU does them) ----
static void ClipTo(RECT* r, LONG w, LONG h)
{
    if (r->left < 0) r->left = 0;
    if (r->top < 0) r->top = 0;
    if (r->right > w) r->right = w;
    if (r->bottom > h) r->bottom = h;
}

// One present copy: every sub-rectangle of the destination, taken from the source at the same offset as SrcRect has
// from DstRect. Both must be 32-bit pictures that are in the segment right now.
static void DoBlt(Device* d, const Op* op)
{
    Alloc *s = op->src, *t = op->dst;
    if (!s || !t) return;
    if ((KindOf(s) == P5A_GDI && BppOf(s->info.format) != 4) || (KindOf(t) == P5A_GDI && BppOf(t->info.format) != 4)) { d->skipped++; return; }
    UCHAR *sp = AnyPtr(d, op->srcSeg, op->srcAddr, s, s->info.bytes), *tp = AnyPtr(d, op->dstSeg, op->dstAddr, t, t->info.bytes);
    if (!sp || !tp || sp == tp) { d->skipped++; return; }
    if ((s->info.kind | t->info.kind) & 0x100) {             // shared surfaces of our Direct3D driver: what travels (first few)
        static volatile LONG said;
        if (InterlockedIncrement(&said) <= 24) {
            ULONG px[2];
            CopyWc((UCHAR*)&px[0], sp, 4);
            CopyWc((UCHAR*)&px[1], sp + (SIZE_T)(s->info.height / 2) * s->info.pitch + (SIZE_T)(s->info.width / 2) * 4, 4);
            TraceF("shared copy kind %X -> %X %ux%u: source corner %08X centre %08X", s->info.kind, t->info.kind, s->info.width, s->info.height, px[0], px[1]);
        }
    }
    LONG dx = op->srcRect.left - op->dstRect.left, dy = op->srcRect.top - op->dstRect.top;
    ULONG n = op->nrects ? op->nrects : 1;
    for (ULONG i = 0; i < n; ++i) {
        RECT r = op->nrects ? op->rects[i] : op->dstRect;
        ClipTo(&r, (LONG)t->info.width, (LONG)t->info.height);
        if (r.left + dx < 0) r.left = -dx;
        if (r.top + dy < 0) r.top = -dy;
        if (r.right + dx > (LONG)s->info.width) r.right = (LONG)s->info.width - dx;
        if (r.bottom + dy > (LONG)s->info.height) r.bottom = (LONG)s->info.height - dy;
        if (r.left >= r.right || r.top >= r.bottom) continue;
        SIZE_T bytes = (SIZE_T)(r.right - r.left) * 4;
        for (LONG y = r.top; y < r.bottom; ++y)
            CopyWc(tp + (SIZE_T)y * t->info.pitch + (SIZE_T)r.left * 4, sp + (SIZE_T)(y + dy) * s->info.pitch + (SIZE_T)(r.left + dx) * 4, bytes);
    }
    d->blts++;
}

static void DoFill(Device* d, const Op* op)
{
    Alloc* t = op->dst;
    if (!t) return;
    if (KindOf(t) == P5A_GDI && BppOf(t->info.format) != 4) { d->skipped++; return; }
    UCHAR* tp = AnyPtr(d, op->dstSeg, op->dstAddr, t, t->info.bytes);
    if (!tp) { d->skipped++; return; }
    ULONG n = op->nrects ? op->nrects : 1;
    for (ULONG i = 0; i < n; ++i) {
        RECT r = op->nrects ? op->rects[i] : op->dstRect;
        ClipTo(&r, (LONG)t->info.width, (LONG)t->info.height);
        for (LONG y = r.top; y < r.bottom; ++y) {
            ULONG* row = (ULONG*)(tp + (SIZE_T)y * t->info.pitch);
            for (LONG x = r.left; x < r.right; ++x) row[x] = op->color;
        }
    }
    d->fills++;
}

// ---- kernel-mode GDI commands (DxgkDdiRenderKm), carried out by the CPU in the engine thread ----
// DdiRenderKm copies the command buffer (with its sub-rectangles) into a blob of ours; the DMA buffer's private data
// carries one Op that names the blob by ticket. DdiPatch writes where every allocation of the list is just before
// the buffer runs; the engine thread then draws.
static const ULONG kGdiAl = 64, kGdiMaxRects = 4096;
struct GdiAl { Alloc* a; ULONG seg, pad; ULONGLONG addr; };
struct GdiCmd { ULONG size, nrects; DXGK_RENDERKM_COMMAND c; };   // RECT rects[nrects] follow
struct GdiBlob { ULONG nal, ncmd; SIZE_T bytes; GdiAl al[kGdiAl]; };   // GdiCmds follow
struct Surf { UCHAR* p; ULONG pitch, w, h, bpp; BOOLEAN wc; };

static UCHAR gRowA[16384 * 4], gRowB[16384 * 4];     // engine thread only

static BOOLEAN GdiSurf(Device* d, GdiBlob* b, UINT idx, Surf* s)
{
    if (idx >= b->nal || !b->al[idx].a) return FALSE;
    Alloc* a = b->al[idx].a;
    s->w = a->info.width;
    s->h = a->info.height;
    s->pitch = a->info.pitch;
    s->bpp = KindOf(a) == P5A_GDI ? BppOf(a->info.format) : 4;
    s->wc = b->al[idx].seg != 2;
    s->p = AnyPtr(d, b->al[idx].seg, b->al[idx].addr, a, (ULONGLONG)s->pitch * s->h);
    return s->p != NULL;
}
static void ReadRow(const Surf* s, LONG x, LONG y, LONG n, UCHAR* out)
{
    const UCHAR* p = s->p + (SIZE_T)y * s->pitch + (SIZE_T)x * s->bpp;
    if (s->wc) CopyWc(out, p, (SIZE_T)n * s->bpp);
    else RtlCopyMemory(out, p, (SIZE_T)n * s->bpp);
}
static ULONG* RowAt(const Surf* s, LONG x, LONG y) { return (ULONG*)(s->p + (SIZE_T)y * s->pitch + (SIZE_T)x * s->bpp); }
static BOOLEAN ClipRect(RECT* r, const Surf* s)
{
    if (r->left < 0) r->left = 0;
    if (r->top < 0) r->top = 0;
    if (r->right > (LONG)s->w) r->right = (LONG)s->w;
    if (r->bottom > (LONG)s->h) r->bottom = (LONG)s->h;
    return r->left < r->right && r->top < r->bottom;
}
// Source coordinate for a destination coordinate, for stretching operations (COLORONCOLOR: truncate).
static LONG MapCoord(LONG v, LONG d0, LONG d1, LONG s0, LONG s1, BOOLEAN mirror)
{
    LONG dn = d1 - d0, sn = s1 - s0;
    if (dn <= 0) return s0;
    LONG o = (LONG)(((LONGLONG)(v - d0) * sn) / dn);
    return mirror ? s1 - 1 - o : s0 + o;
}

static void GdiSay(Device* d, ULONG op, const char* what)
{
    static volatile LONG said[8];
    if (op < 8 && InterlockedIncrement(&said[op]) <= 3) TraceF("gdi op %u: %s", op, what);
}

static void GdiBitBlt(Device* d, GdiBlob* b, const DXGK_GDIARG_BITBLT* c, const RECT* rects, ULONG n)
{
    Surf s, t;
    if (!GdiSurf(d, b, c->SrcAllocationIndex, &s) || !GdiSurf(d, b, c->DstAllocationIndex, &t) || s.bpp != 4 || t.bpp != 4) { d->gdiSkips++; return; }
    if (c->Rop < DXGK_GDIROP_SRCCOPY || c->Rop > DXGK_GDIROP_SRCOR) { GdiSay(d, 1, "unsupported rop"); d->gdiSkips++; return; }
    LONG dx = c->SrcRect.left - c->DstRect.left, dy = c->SrcRect.top - c->DstRect.top;
    for (ULONG i = 0; i < (n ? n : 1); ++i) {
        RECT r = n ? rects[i] : c->DstRect;
        if (!ClipRect(&r, &t)) continue;
        if (r.left + dx < 0) r.left = -dx;
        if (r.top + dy < 0) r.top = -dy;
        if (r.right + dx > (LONG)s.w) r.right = (LONG)s.w - dx;
        if (r.bottom + dy > (LONG)s.h) r.bottom = (LONG)s.h - dy;
        if (r.left >= r.right || r.top >= r.bottom) continue;
        LONG w = r.right - r.left;
        BOOLEAN up = s.p == t.p && dy < 0;               // same surface, source above: bottom row first
        for (LONG k = 0; k < r.bottom - r.top; ++k) {
            LONG y = up ? r.bottom - 1 - k : r.top + k;
            ReadRow(&s, r.left + dx, y + dy, w, gRowA);
            ULONG* sp = (ULONG*)gRowA;
            if (c->Rop != DXGK_GDIROP_SRCCOPY) {
                ReadRow(&t, r.left, y, w, gRowB);
                ULONG* tp = (ULONG*)gRowB;
                for (LONG x = 0; x < w; ++x)
                    sp[x] = c->Rop == DXGK_GDIROP_SRCINVERT ? tp[x] ^ sp[x] : c->Rop == DXGK_GDIROP_SRCAND ? tp[x] & sp[x] : tp[x] | sp[x];
            }
            RtlCopyMemory(RowAt(&t, r.left, y), sp, (SIZE_T)w * 4);
        }
    }
}

static void GdiColorFill(Device* d, GdiBlob* b, const DXGK_GDIARG_COLORFILL* c, const RECT* rects, ULONG n)
{
    Surf t;
    if (!GdiSurf(d, b, c->DstAllocationIndex, &t) || t.bpp != 4) { d->gdiSkips++; return; }
    if (c->Rop < 1 || c->Rop > 6) { GdiSay(d, 2, "unsupported rop"); d->gdiSkips++; return; }
    ULONG col = c->Color;
    for (ULONG i = 0; i < (n ? n : 1); ++i) {
        RECT r = n ? rects[i] : c->DstRect;
        if (!ClipRect(&r, &t)) continue;
        LONG w = r.right - r.left;
        ULONG* row = (ULONG*)gRowA;
        if (c->Rop == 1) for (LONG x = 0; x < w; ++x) row[x] = col;
        for (LONG y = r.top; y < r.bottom; ++y) {
            if (c->Rop != 1) {
                ReadRow(&t, r.left, y, w, gRowA);
                for (LONG x = 0; x < w; ++x) {
                    ULONG v = row[x];
                    switch (c->Rop) {
                    case 2: v ^= col; break;                     // PATINVERT
                    case 3: v = ~(v ^ col); break;               // PDXN
                    case 4: v = ~v; break;                       // DSTINVERT
                    case 5: v &= col; break;                     // PATAND
                    case 6: v |= col; break;                     // PATOR
                    }
                    row[x] = v;
                }
            }
            RtlCopyMemory(RowAt(&t, r.left, y), row, (SIZE_T)w * 4);
        }
    }
}

static inline ULONG Ch(ULONG v, int sh) { return (v >> sh) & 0xFF; }

static void GdiAlphaBlend(Device* d, GdiBlob* b, const DXGK_GDIARG_ALPHABLEND* c, const RECT* rects, ULONG n)
{
    Surf s, t;
    if (!GdiSurf(d, b, c->SrcAllocationIndex, &s) || !GdiSurf(d, b, c->DstAllocationIndex, &t) || s.bpp != 4 || t.bpp != 4) { d->gdiSkips++; return; }
    ULONG sca = c->SourceConstantAlpha;
    for (ULONG i = 0; i < (n ? n : 1); ++i) {
        RECT r = n ? rects[i] : c->DstRect;
        if (!ClipRect(&r, &t)) continue;
        LONG w = r.right - r.left;
        for (LONG y = r.top; y < r.bottom; ++y) {
            LONG sy = MapCoord(y, c->DstRect.top, c->DstRect.bottom, c->SrcRect.top, c->SrcRect.bottom, FALSE);
            if (sy < 0 || sy >= (LONG)s.h) continue;
            LONG sx0 = c->SrcRect.left < 0 ? 0 : c->SrcRect.left, sx1 = c->SrcRect.right > (LONG)s.w ? (LONG)s.w : c->SrcRect.right;
            if (sx0 >= sx1) continue;
            ReadRow(&s, sx0, sy, sx1 - sx0, gRowA);
            ReadRow(&t, r.left, y, w, gRowB);
            ULONG *sp = (ULONG*)gRowA, *tp = (ULONG*)gRowB;
            for (LONG x = 0; x < w; ++x) {
                LONG sx = MapCoord(r.left + x, c->DstRect.left, c->DstRect.right, c->SrcRect.left, c->SrcRect.right, FALSE);
                if (sx < sx0 || sx >= sx1) continue;
                ULONG sv = sp[sx - sx0], dv = tp[x], out = 0;
                if (c->SourceHasAlpha) {                     // premultiplied source (AC_SRC_ALPHA)
                    ULONG sa = (Ch(sv, 24) * sca + 127) / 255;
                    for (int sh = 0; sh < 32; sh += 8) {
                        ULONG v = (Ch(sv, sh) * sca + 127) / 255 + (Ch(dv, sh) * (255 - sa) + 127) / 255;
                        out |= (v > 255 ? 255 : v) << sh;
                    }
                } else {
                    for (int sh = 0; sh < 32; sh += 8) out |= ((Ch(sv, sh) * sca + Ch(dv, sh) * (255 - sca) + 127) / 255) << sh;
                }
                tp[x] = out;
            }
            RtlCopyMemory(RowAt(&t, r.left, y), tp, (SIZE_T)w * 4);
        }
    }
}

// StretchBlt and TransparentBlt: per-pixel source lookup (nearest), optional colour key.
static void GdiStretch(Device* d, GdiBlob* b, UINT si, UINT di, const RECT* src, const RECT* dst, BOOLEAN mx, BOOLEAN my, BOOLEAN key, ULONG keyColor,
                       ULONG keyMask, const RECT* rects, ULONG n)
{
    Surf s, t;
    if (!GdiSurf(d, b, si, &s) || !GdiSurf(d, b, di, &t) || s.bpp != 4 || t.bpp != 4) { d->gdiSkips++; return; }
    LONG sx0 = src->left < 0 ? 0 : src->left, sx1 = src->right > (LONG)s.w ? (LONG)s.w : src->right;
    if (sx0 >= sx1) return;
    for (ULONG i = 0; i < (n ? n : 1); ++i) {
        RECT r = n ? rects[i] : *dst;
        if (!ClipRect(&r, &t)) continue;
        LONG w = r.right - r.left;
        for (LONG y = r.top; y < r.bottom; ++y) {
            LONG sy = MapCoord(y, dst->top, dst->bottom, src->top, src->bottom, my);
            if (sy < 0 || sy >= (LONG)s.h) continue;
            ReadRow(&s, sx0, sy, sx1 - sx0, gRowA);
            ULONG *sp = (ULONG*)gRowA, *tp = (ULONG*)gRowB;
            if (key) ReadRow(&t, r.left, y, w, gRowB);
            for (LONG x = 0; x < w; ++x) {
                LONG sx = MapCoord(r.left + x, dst->left, dst->right, src->left, src->right, mx);
                if (sx < sx0 || sx >= sx1) { if (!key) tp[x] = 0; continue; }
                ULONG v = sp[sx - sx0];
                if (key && (v & keyMask) == (keyColor & keyMask)) continue;
                tp[x] = v;
            }
            RtlCopyMemory(RowAt(&t, r.left, y), tp, (SIZE_T)w * 4);
        }
    }
}

// ClearType text: Microsoft's formula (DXGK_GDIARG_CLEARTYPEBLEND remarks). The alpha surface holds per-channel
// coverage in the destination's coordinate space shifted by DstToAlphaOffset; the gamma surface is 16 rows of
// [256 gamma | 256 inverse gamma] bytes.
static void GdiClearType(Device* d, GdiBlob* b, const DXGK_GDIARG_CLEARTYPEBLEND* c, const RECT* rects, ULONG n)
{
    Surf t, al, g;
    if (!GdiSurf(d, b, c->DstAllocationIndex, &t) || !GdiSurf(d, b, c->AlphaSurfAllocationIndex, &al) || t.bpp != 4) { d->gdiSkips++; return; }
    BOOLEAN haveGamma = c->Gamma <= 15 && GdiSurf(d, b, c->GammaSurfAllocationIndex, &g) && g.bpp == 1 && g.w >= 512 && g.h > c->Gamma;
    static volatile LONG said;
    if (InterlockedIncrement(&said) <= 4)
        TraceF("cleartype dst %ld,%ld-%ld,%ld rects %u alpha %ux%u bpp %u pitch %u/%u offset %d,%d gamma %u (%s) color %08X/%08X", c->DstRect.left, c->DstRect.top,
               c->DstRect.right, c->DstRect.bottom, n, al.w, al.h, al.bpp, al.pitch, c->AlphaSurfPitch, c->DstToAlphaOffsetX, c->DstToAlphaOffsetY, c->Gamma,
               haveGamma ? "table" : "none", c->Color, c->Color2);
    if (c->AlphaSurfPitch) al.pitch = c->AlphaSurfPitch;
    if ((ULONGLONG)al.pitch * al.h > (ULONGLONG)b->al[c->AlphaSurfAllocationIndex].a->info.bytes) al.pitch = b->al[c->AlphaSurfAllocationIndex].a->info.pitch;
    UCHAR gt[256], igt[256];
    if (haveGamma) {
        ReadRow(&g, 0, (LONG)c->Gamma, 256, gt);
        ReadRow(&g, 256, (LONG)c->Gamma, 256, igt);
    }
    for (ULONG i = 0; i < (n ? n : 1); ++i) {
        RECT r = n ? rects[i] : c->DstRect;
        if (!ClipRect(&r, &t)) continue;
        LONG w = r.right - r.left;
        for (LONG y = r.top; y < r.bottom; ++y) {
            LONG ay = y + c->DstToAlphaOffsetY, ax0 = r.left + c->DstToAlphaOffsetX;
            if (ay < 0 || ay >= (LONG)al.h || ax0 < 0 || ax0 + w > (LONG)al.w) { d->gdiSkips++; continue; }
            ReadRow(&al, ax0, ay, w, gRowA);
            ReadRow(&t, r.left, y, w, gRowB);
            ULONG* tp = (ULONG*)gRowB;
            for (LONG x = 0; x < w; ++x) {
                ULONG av = al.bpp == 4 ? ((ULONG*)gRowA)[x] : al.bpp == 1 ? gRowA[x] * 0x010101u : 0;
                ULONG dv = tp[x], out = dv & 0xFF000000u;
                for (int sh = 0; sh < 24; sh += 8) {
                    ULONG A = Ch(av, sh), D = Ch(dv, sh), C = Ch(c->Color, sh), v;
                    if (A == 0) v = D;
                    else if (A == 255) v = Ch(c->Color2, sh);
                    else if (haveGamma) {
                        LONG tmp = gt[D];
                        LONG m = (tmp * 255 + ((LONG)C - tmp) * (LONG)A + 127) / 255;
                        v = igt[m < 0 ? 0 : m > 255 ? 255 : m];
                    } else {
                        LONG m = ((LONG)D * 255 + ((LONG)C - (LONG)D) * (LONG)A + 127) / 255;
                        v = (ULONG)(m < 0 ? 0 : m > 255 ? 255 : m);
                    }
                    out |= v << sh;
                }
                tp[x] = out;
            }
            RtlCopyMemory(RowAt(&t, r.left, y), tp, (SIZE_T)w * 4);
        }
    }
}

static void DoGdi(Device* d, GdiBlob* b)
{
    UCHAR* p = (UCHAR*)(b + 1);
    UCHAR* end = (UCHAR*)b + b->bytes;
    for (ULONG k = 0; k < b->ncmd && p + sizeof(GdiCmd) <= end; ++k) {
        GdiCmd* g = (GdiCmd*)p;
        if (g->size < sizeof(GdiCmd) || g->size > (SIZE_T)(end - p)) break;
        const RECT* rects = (const RECT*)(g + 1);
        const DXGK_RENDERKM_COMMAND* c = &g->c;
        ULONG op = (ULONG)c->OpCode;
        if (op < 8) d->gdiOps[op]++;
        switch (c->OpCode) {
        case DXGK_GDIOP_BITBLT: GdiBitBlt(d, b, &c->Command.BitBlt, rects, g->nrects); break;
        case DXGK_GDIOP_COLORFILL: GdiColorFill(d, b, &c->Command.ColorFill, rects, g->nrects); break;
        case DXGK_GDIOP_ALPHABLEND: GdiAlphaBlend(d, b, &c->Command.AlphaBlend, rects, g->nrects); break;
        case DXGK_GDIOP_STRETCHBLT: {
            const DXGK_GDIARG_STRETCHBLT* s = &c->Command.StretchBlt;
            GdiStretch(d, b, s->SrcAllocationIndex, s->DstAllocationIndex, &s->SrcRect, &s->DstRect, s->MirrorX, s->MirrorY, FALSE, 0, 0, rects, g->nrects);
            break;
        }
        case DXGK_GDIOP_TRANSPARENTBLT: {
            const DXGK_GDIARG_TRANSPARENTBLT* s = &c->Command.TransparentBlt;
            GdiStretch(d, b, s->SrcAllocationIndex, s->DstAllocationIndex, &s->SrcRect, &s->DstRect, FALSE, FALSE, TRUE, s->Color,
                       s->Flags.HonorAlpha ? 0xFFFFFFFFu : 0x00FFFFFFu, rects, g->nrects);
            break;
        }
        case DXGK_GDIOP_CLEARTYPEBLEND: GdiClearType(d, b, &c->Command.ClearTypeBlend, rects, g->nrects); break;
        default: break;                                      // escape and unknown: nothing to do
        }
        p += g->size;
    }
}

// The page numbers of `pages` pages from page `first` of a page list, or NULL if the list is shorter than that.
static const PFN_NUMBER* MdlPages(PMDL mdl, ULONG first, SIZE_T pages)
{
    if (!mdl || (SIZE_T)first + pages > ADDRESS_AND_SIZE_TO_SPAN_PAGES(MmGetMdlVirtualAddress(mdl), MmGetMdlByteCount(mdl))) return NULL;
    return MmGetMdlPfnArray(mdl) + first;
}

// What Windows' memory manager asks for: an allocation's content into the segment, out of it, to another place in
// it, or a fill. The system-memory side comes as a page list, mapped here a piece at a time.
static void DoPage(Device* d, const Op* op, const PFN_NUMBER* own)
{
    switch (op->kind) {
    case OP_AP_MAP: {
        Alloc* a = op->dst;
        SIZE_T pages = (SIZE_T)(op->bytes / PAGE_SIZE);
        const PFN_NUMBER* pfn = own ? own : MdlPages(op->mdl, op->mdlOffset, pages);
        UnmapAp(a);
        if (!a || !pfn || !pages) { d->apFails++; break; }
        PMDL m = IoAllocateMdl(NULL, (ULONG)(pages * PAGE_SIZE), FALSE, FALSE, NULL);
        if (!m) { d->apFails++; break; }
        RtlCopyMemory(MmGetMdlPfnArray(m), pfn, pages * sizeof(PFN_NUMBER));
        m->MdlFlags |= MDL_PAGES_LOCKED;
        UCHAR* va = (UCHAR*)MmMapLockedPagesSpecifyCache(m, KernelMode, MmCached, NULL, FALSE, NormalPagePriority | MdlMappingNoExecute);
        if (!va) { IoFreeMdl(m); d->apFails++; break; }
        a->mdl = m;
        a->va = va;
        a->pages = pages;
        a->apAddr = op->dstAddr;
        d->apMaps++;
        break;
    }
    case OP_AP_UNMAP:
        UnmapAp(op->dst);
        d->apUnmaps++;
        break;
    case OP_PAGE_FILL: {
        ULONG* p = (ULONG*)AnyPtr(d, op->dstSeg, op->dstAddr, op->dst, op->bytes);
        if (!p) { d->pageFails++; break; }
        for (SIZE_T i = 0; i < op->bytes / 4; ++i) p[i] = op->color;
        d->pageFills++;
        break;
    }
    case OP_PAGE_MOVE: {
        UCHAR *s = AnyPtr(d, op->srcSeg, op->srcAddr, op->src, op->bytes), *t = AnyPtr(d, op->dstSeg, op->dstAddr, op->dst, op->bytes);
        if (!s || !t) { d->pageFails++; break; }
        if (t + op->bytes <= s || s + op->bytes <= t) CopyWc(t, s, (SIZE_T)op->bytes);
        else RtlMoveMemory(t, s, (SIZE_T)op->bytes);
        d->pageMoves++;
        break;
    }
    case OP_PAGE_IN:
    case OP_PAGE_OUT: {
        BOOLEAN in = op->kind == OP_PAGE_IN;
        UCHAR* seg = in ? AnyPtr(d, op->dstSeg, op->dstAddr, op->dst, op->bytes) : AnyPtr(d, op->srcSeg, op->srcAddr, op->src, op->bytes);
        SIZE_T pages = (SIZE_T)((op->bytes + PAGE_SIZE - 1) / PAGE_SIZE);
        const PFN_NUMBER* pfn = own ? own : MdlPages(op->mdl, op->mdlOffset, pages);
        if (!seg || !pfn || !pages) { d->pageFails++; break; }
        const SIZE_T kChunk = 1024;                              // 4 MiB of address space at a time
        PMDL m = IoAllocateMdl(NULL, (ULONG)(kChunk * PAGE_SIZE), FALSE, FALSE, NULL);
        if (!m) { d->pageFails++; break; }
        SIZE_T done = 0;
        while (done < pages) {
            SIZE_T n = pages - done < kChunk ? pages - done : kChunk;
            MmInitializeMdl(m, NULL, n * PAGE_SIZE);
            RtlCopyMemory(MmGetMdlPfnArray(m), pfn + done, n * sizeof(PFN_NUMBER));
            m->MdlFlags |= MDL_PAGES_LOCKED;
            UCHAR* va = (UCHAR*)MmMapLockedPagesSpecifyCache(m, KernelMode, MmCached, NULL, FALSE, NormalPagePriority | MdlMappingNoExecute);
            if (!va) { d->pageFails++; break; }
            SIZE_T off = done * PAGE_SIZE, bytes = n * PAGE_SIZE;
            if (bytes > op->bytes - off) bytes = (SIZE_T)(op->bytes - off);
            if (in) RtlCopyMemory(seg + off, va, bytes);
            else CopyWc(va, seg + off, bytes);
            MmUnmapLockedPages(va, m);
            done += n;
        }
        IoFreeMdl(m);
        if (in) d->pageIns++; else d->pageOuts++;
        break;
    }
    default:
        break;
    }
}

// Only when flips are not possible: the picture Windows wants on the screen, copied into the frame buffer.
static void ShowSource(Device* d)
{
    if (d->flipOk || !d->fb || !d->visible || !d->source) return;
    const UCHAR* s = SegPtr(d, (ULONGLONG)d->sourceAddress.QuadPart, d->fbBytes);
    if (!s) return;
    for (ULONG y = 0; y < d->height; ++y) CopyWc(d->fb + (SIZE_T)y * d->pitch, s + (SIZE_T)y * d->pitch, (SIZE_T)d->width * 4);
    d->copies++;
}

// ---- the engine thread: submitted work in order, and the 60 Hz tick ----
static VOID EngineThread(PVOID context)
{
    Device* d = (Device*)context;
    LARGE_INTEGER freq;
    LONGLONG next = KeQueryPerformanceCounter(&freq).QuadPart, lastLog = next;
    // One tick per frame of the real refresh rate (MeasureRefresh): period = freq * den / num.
    LONGLONG period = (LONGLONG)((ULONGLONG)freq.QuadPart * (d->refreshDen ? d->refreshDen : 1) / (d->refreshNum ? d->refreshNum : 60));
    ExSetTimerResolution(10000, TRUE);                       // 1 ms, so the tick is not rounded to 15.6 ms
    LogF("engine start");
    static Work w;                                           // one engine thread; too big for its stack
    while (d->started) {
        // Submitted buffers, oldest first.
        for (;;) {
            KIRQL irql;
            BOOLEAN have = FALSE;
            KeAcquireSpinLock(&d->queueLock, &irql);
            if (d->queueTail != d->queueHead) {
                w = d->queue[d->queueTail];
                d->queueTail = (d->queueTail + 1) % kQueue;
                have = TRUE;
            }
            KeReleaseSpinLock(&d->queueLock, irql);
            if (!have) break;
            if (w.op.kind == OP_BLT) DoBlt(d, &w.op);
            else if (w.op.kind == OP_FILL) DoFill(d, &w.op);
            else if (w.op.kind == OP_GDI) {
                GdiBlob* b = (GdiBlob*)TakeTicket(d, w.op.ticket);
                if (b) {
                    DoGdi(d, b);
                    ExFreePoolWithTag(b, P5_TAG);
                }
            } else if (w.op.kind >= OP_PAGE_IN && w.op.kind <= OP_AP_UNMAP) {
                PFN_NUMBER* own = TakeTicket(d, w.op.ticket);
                w.op.mdl = NULL;                             // never Windows' list here: it may be gone by now
                DoPage(d, &w.op, own);
                if (own) ExFreePoolWithTag(own, P5_TAG);
            }
            if (!w.last) continue;
            DXGKARGCB_NOTIFY_INTERRUPT_DATA data;
            RtlZeroMemory(&data, sizeof data);
            if (w.op.kind == OP_PREEMPT) {
                data.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
                data.DmaPreempted.PreemptionFenceId = w.fence;
                data.DmaPreempted.LastCompletedFenceId = (ULONG)d->lastCompleted;
                data.DmaPreempted.NodeOrdinal = 0;
                data.DmaPreempted.EngineOrdinal = 0;
                d->preempts++;
            } else {
                InterlockedExchange(&d->lastCompleted, (LONG)w.fence);
                data.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
                data.DmaCompleted.SubmissionFenceId = w.fence;
                data.DmaCompleted.NodeOrdinal = 0;
                data.DmaCompleted.EngineOrdinal = 0;
            }
            Notify(d, &data);
        }
        // The tick.
        LONGLONG now = KeQueryPerformanceCounter(NULL).QuadPart;
        if (now >= next) {
            next += period;
            if (now > next + 4 * period) next = now + period;         // we were held up: do not try to catch up
            ShowSource(d);
            InterlockedExchange(&d->sourceChanged, 0);
            d->vsyncs++;
            if (d->vsyncOn) {
                DXGKARGCB_NOTIFY_INTERRUPT_DATA data;
                RtlZeroMemory(&data, sizeof data);
                data.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
                data.CrtcVsync.VidPnTargetId = 0;
                data.CrtcVsync.PhysicalAddress = d->sourceAddress;
                Notify(d, &data);
            }
            if (now - lastLog > 2 * freq.QuadPart) {
                lastLog = now;
                RegSet(L"FullVsyncs", d->vsyncs, FALSE);
                RegSet(L"FullCopies", d->copies, FALSE);
                RegSet(L"FullSubmitted", (ULONG)d->lastSubmitted, FALSE);
                RegSet(L"FullCompleted", (ULONG)d->lastCompleted, FALSE);
                RegSet(L"FullPresents", d->presents3, FALSE);
                RegSet(L"FullBlts", d->blts, FALSE);
                RegSet(L"FullFlips", d->flipsDone, FALSE);
                RegSet(L"FullRenders", d->renders, FALSE);
                RegSet(L"FullAllocs", (ULONG)d->allocs, FALSE);
                RegSet(L"FullDevices", (ULONG)d->devices, FALSE);
                RegSet(L"FullDropped", d->dropped, FALSE);
                LogFlush();
                static ULONG beats;
                if (++beats <= 60 || beats % 30 == 0)
                    TraceF("alive: ticks %u submitted %ld completed %ld presents %u blts %u fills %u skipped %u flips %u (held %u) copies %u renders %u allocs %ld devices %ld contexts %ld | page in %u out %u move %u fill %u FAIL %u dropped %u",
                           d->vsyncs, d->lastSubmitted, d->lastCompleted, d->presents3, d->blts, d->fills, d->skipped, d->flipsDone, d->flipsHeld, d->copies, d->renders, d->allocs,
                           d->devices, d->contexts, d->pageIns, d->pageOuts, d->pageMoves, d->pageFills, d->pageFails, d->dropped);
                if (gGdiAccel)
                    TraceF("gdi: renderkm %u | blt %u fill %u alpha %u stretch %u transparent %u cleartype %u | skipped %u lost %u | aperture map %u unmap %u fail %u",
                           d->renderKms, d->gdiOps[1], d->gdiOps[2], d->gdiOps[3], d->gdiOps[4], d->gdiOps[6], d->gdiOps[7], d->gdiSkips, d->gdiNoTicket, d->apMaps, d->apUnmaps, d->apFails);
            }
        }
        LONGLONG left = next - KeQueryPerformanceCounter(NULL).QuadPart;
        if (left > 0) {
            LARGE_INTEGER wait;
            wait.QuadPart = -(left * 10000000LL / freq.QuadPart);
            if (wait.QuadPart > -1000) wait.QuadPart = -1000;
            KeWaitForSingleObject(&d->engineWake, Executive, KernelMode, FALSE, &wait);
        }
    }
    ExSetTimerResolution(0, FALSE);
    LogF("engine stop");
    LogFlush();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// Forget what is queued (a reset, a stop). Caller holds the queue lock or is the only one left.
static void DiscardQueue(Device* d)
{
    while (d->queue && d->queueTail != d->queueHead) {
        PFN_NUMBER* own = TakeTicketLocked(d, d->queue[d->queueTail].op.ticket);
        if (own) ExFreePoolWithTag(own, P5_TAG);
        d->queueTail = (d->queueTail + 1) % kQueue;
    }
}

static void StartEngine(Device* d)
{
    KeInitializeEvent(&d->engineWake, SynchronizationEvent, FALSE);
    KeInitializeSpinLock(&d->queueLock);
    d->queue = (Work*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(Work) * kQueue, P5_TAG);
    d->queueHead = d->queueTail = 0;
    d->visible = 1;
    HANDLE thread = NULL;
    if (d->queue && NT_SUCCESS(PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, EngineThread, d))) {
        if (!NT_SUCCESS(ObReferenceObjectByHandle(thread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID*)&d->engine, NULL))) d->engine = NULL;
        ZwClose(thread);
    }
}

static void StopEngine(Device* d)
{
    if (d->engine) {
        WakeEngine(d);
        KeWaitForSingleObject(d->engine, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(d->engine);
        d->engine = NULL;
    }
    DiscardQueue(d);
    for (ULONG i = 0; i < kTickets; ++i) {                   // copies whose buffer was never submitted
        if (d->tickets[i].pfns) ExFreePoolWithTag(d->tickets[i].pfns, P5_TAG);
        d->tickets[i].id = 0;
        d->tickets[i].pfns = NULL;
    }
    if (d->queue) ExFreePoolWithTag(d->queue, P5_TAG);
    d->queue = NULL;
}

// ---- the GPU-memory range Windows gets, the page with libps5gpu's screen record, the display's state ----
// Range: from 64 MiB into the GPU's memory up to the frame buffer (at 240 MiB on this console: 176 MiB). It has to
// lie in the GPU's PCI memory window (BAR0: the first 256 MiB of the GPU's memory), because that window is how
// Windows itself reaches a CPU-visible segment: with a range higher up (tried first, 2026-11-20) Windows dropped the
// primary right after creating it. `ps5gpu2 vramscan` showed the first 49 MiB holding data Linux left (unchanging)
// and everything from there to the frame buffer never written; libps5gpu's heap starts at 512 MiB.
static const ULONGLONG kSegOffInVram = 0x4000000ull, kBarBytes = 0x10000000ull, kScanRecPageOff = 0x14000ull;

static void ReleaseVram(Device* d)
{
    if (d->segVa) MmUnmapIoSpace(d->segVa, (SIZE_T)d->segBytes);
    if (d->scanPage) MmUnmapIoSpace(d->scanPage, PAGE_SIZE);
    d->segVa = NULL;
    d->scanPage = NULL;
    d->scanRec = NULL;
    d->flipOk = FALSE;
}

// The device's memory window that holds `address`: its start, or 0.
static ULONGLONG MemoryWindowOf(Device* d, ULONGLONG address, ULONGLONG* length)
{
    DXGK_DEVICE_INFO info;
    if (!NT_SUCCESS(d->dxgk.DxgkCbGetDeviceInformation(d->dxgk.DeviceHandle, &info)) || !info.TranslatedResourceList || !info.TranslatedResourceList->Count) return 0;
    PCM_FULL_RESOURCE_DESCRIPTOR full = &info.TranslatedResourceList->List[0];
    for (ULONG i = 0; i < full->PartialResourceList.Count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = &full->PartialResourceList.PartialDescriptors[i];
        if (r->Type != CmResourceTypeMemory && r->Type != CmResourceTypeMemoryLarge) continue;
        ULONGLONG len = 0;
        ULONGLONG start = (ULONGLONG)r->u.Generic.Start.QuadPart;
        len = RtlCmDecodeMemIoResource(r, &start);
        if (address >= start && address - start < len) { *length = len; return start; }
    }
    return 0;
}

static BOOLEAN SetupVram(Device* d)
{
    if (!d->regs) { LogF("vram: no registers (not the PS5's GPU)"); return FALSE; }
    ULONG fbBase = RegRead(d, rFB_LOCATION_BASE), fbTop = RegRead(d, rFB_LOCATION_TOP), fbOffset = RegRead(d, rFB_OFFSET);
    ULONGLONG mcBase = (ULONGLONG)(fbBase & 0xFFFFFF) << 24, mcTop = ((ULONGLONG)(fbTop & 0xFFFFFF) << 24) | 0xFFFFFF;
    ULONGLONG vram = mcTop - mcBase + 1, carve = (ULONGLONG)fbOffset << 24;
    if (carve < 0x100000000ull || vram < 0x40000000ull || vram > 0x400000000ull || mcTop < mcBase) { LogF("vram: layout not understood (%08X %08X %08X)", fbBase, fbTop, fbOffset); return FALSE; }
    d->fbPhys = (ULONGLONG)d->disp.PhysicAddress.QuadPart;
    LookAtDisplay(d);
    // The frame buffer is in the GPU's memory at (display address - carve), and the CPU reaches it through the
    // memory window at the same offset: that gives the window's start.
    ULONGLONG winLen = 0, win = MemoryWindowOf(d, d->fbPhys, &winLen);
    ULONGLONG fbOff = d->fbScan >= carve ? d->fbScan - carve : ~0ull;
    if (!win || winLen < kBarBytes || fbOff >= kBarBytes || d->fbPhys - win != fbOff || fbOff < kSegOffInVram + 0x2000000ull) {
        LogF("vram: frame buffer %I64X (display %I64X) does not sit in the memory window as expected (window %I64X + %I64X)", d->fbPhys, d->fbScan, win, winLen);
        return FALSE;
    }
    d->segBytes = (fbOff - kSegOffInVram) & ~0xFFFFFFull;
    d->segPhys = carve + kSegOffInVram;
    d->segCpu = win + kSegOffInVram;
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)d->segPhys;
    d->segVa = (UCHAR*)MmMapIoSpace(pa, (SIZE_T)d->segBytes, MmWriteCombined);
    if (!d->segVa) { LogF("vram: the segment cannot be mapped"); return FALSE; }
    pa.QuadPart = (LONGLONG)(carve + vram - kAreaBelowTop + kScanRecPageOff);
    d->scanPage = (UCHAR*)MmMapIoSpace(pa, PAGE_SIZE, MmWriteCombined);
    d->scanRec = d->scanPage ? (ScanRec*)(d->scanPage + 0x100) : NULL;
    d->pagingSegment = RegGet(L"PagingSegment", 0) ? 1 : 0;
    LogF("vram: carve %I64X, %I64X bytes; segment %I64X (CPU %I64X) + %I64X; paging buffers in segment %u", carve, vram, d->segPhys, d->segCpu, d->segBytes, d->pagingSegment);
    if (RegGet(L"NoFlip", 0)) d->flipOk = FALSE;
    return TRUE;
}

// ---- adapter questions ----
struct P5AdapterInfo { ULONG magic, version, width, height, pitch, reserved; ULONGLONG fbPhys, segPhys, segBytes; };

static NTSTATUS APIENTRY FullQueryAdapterInfo(IN_CONST_HANDLE context, IN_CONST_PDXGKARG_QUERYADAPTERINFO q)
{
    Tr tr("FullQueryAdapterInfo");
    Device* d = (Device*)context;
    switch (q->Type) {
    case DXGKQAITYPE_UMDRIVERPRIVATE: {
        if (q->OutputDataSize < sizeof(P5AdapterInfo)) return STATUS_BUFFER_TOO_SMALL;
        P5AdapterInfo* info = (P5AdapterInfo*)q->pOutputData;
        RtlZeroMemory(info, sizeof *info);
        info->magic = P5A_MAGIC;
        info->version = 4;
        info->segPhys = d->segPhys;
        info->segBytes = d->segBytes;
        info->width = d->width;
        info->height = d->height;
        info->pitch = d->pitch;
        info->fbPhys = d->full ? d->fbScan : (ULONGLONG)d->disp.PhysicAddress.QuadPart;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_DRIVERCAPS: {
        LogF("caps asked: %u bytes given, %u in our headers", q->OutputDataSize, (ULONG)sizeof(DXGK_DRIVERCAPS));
        if (q->OutputDataSize < FIELD_OFFSET(DXGK_DRIVERCAPS, GpuEngineTopology) + sizeof(DXGK_GPUENGINETOPOLOGY)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_DRIVERCAPS* caps = (DXGK_DRIVERCAPS*)q->pOutputData;
        RtlZeroMemory(caps, q->OutputDataSize);
        caps->WDDMVersion = gWddm2 ? DXGKDDI_WDDMv2 : DXGKDDI_WDDMv1_3;
        caps->HighestAcceptableAddress.QuadPart = -1;
        caps->FlipCaps.FlipOnVSyncMmIo = TRUE;
        caps->MaxQueuedFlipOnVSync = 1;
        caps->PresentationCaps.NoScreenToScreenBlt = 1;      // our copies are one-directional (CopyWc)
        caps->PresentationCaps.NoOverlapScreenBlt = 1;
        caps->SchedulingCaps.MultiEngineAware = 1;
        caps->SchedulingCaps.PreemptionAware = 1;
        caps->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
        caps->SupportNonVGA = TRUE;
        caps->SupportSmoothRotation = FALSE;
        gGdi = RegGetSession(L"GdiInterop", 0);
        gGdiAccel = RegGetSession(L"GdiAccel", 0);
        gGdiTexCpu = RegGetSession(L"GdiTexCpu", 1);
        if (gGdiAccel) {
            gGdi = 0;
            caps->PresentationCaps.SupportKernelModeCommandBuffer = 1;
            caps->PresentationCaps.AlignmentShift = 2;
            caps->PresentationCaps.MaxTextureWidthShift = 3;
            caps->PresentationCaps.MaxTextureHeightShift = 3;
            caps->PresentationCaps.NoSameBitmapAlphaBlend = 1;
            caps->PresentationCaps.NoSameBitmapStretchBlt = 1;
            caps->PresentationCaps.NoSameBitmapTransparentBlt = 1;
            caps->PresentationCaps.NoTempSurfaceForClearTypeBlend = 1;
        }
        if (gGdi) {
            caps->PresentationCaps.DriverSupportsCddDwmInterop = 1;
            caps->PresentationCaps.SupportSoftwareDeviceBitmaps = (gGdi & 2) ? 0 : 1;
            caps->PresentationCaps.AlignmentShift = 2;
            caps->PresentationCaps.MaxTextureWidthShift = 3;
            caps->PresentationCaps.MaxTextureHeightShift = 3;
        }
        LogF("caps: GDI interop %u, GDI acceleration %u (textures CPU-visible %u)", gGdi, gGdiAccel, gGdiTexCpu);
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_QUERYSEGMENT: {
        if (q->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_QUERYSEGMENTOUT* out = (DXGK_QUERYSEGMENTOUT*)q->pOutputData;
        if (!out->pSegmentDescriptor) { out->NbSegment = 1; return STATUS_SUCCESS; }
        DXGK_SEGMENTDESCRIPTOR* seg = out->pSegmentDescriptor;
        RtlZeroMemory(seg, sizeof *seg);
        seg->BaseAddress.QuadPart = (LONGLONG)d->segPhys;    // addresses Windows gives us are what the display can be pointed at
        seg->CpuTranslatedAddress.QuadPart = (LONGLONG)d->segCpu;
        seg->Flags.CpuVisible = TRUE;
        seg->Size = (SIZE_T)d->segBytes;
        seg->CommitLimit = (SIZE_T)d->segBytes;
        out->PagingBufferSegmentId = d->pagingSegment;
        out->PagingBufferSize = PAGE_SIZE;
        out->PagingBufferPrivateDataSize = sizeof(Op) * 32;
        LogF("segment v1 asked");
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_QUERYSEGMENT3: {
        if (q->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT3)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_QUERYSEGMENTOUT3* out = (DXGK_QUERYSEGMENTOUT3*)q->pOutputData;
        if (!out->pSegmentDescriptor) { out->NbSegment = gGdiAccel ? 2 : 1; return STATUS_SUCCESS; }
        DXGK_SEGMENTDESCRIPTOR3* seg = out->pSegmentDescriptor;
        if (gGdiAccel && out->NbSegment >= 2) {
            DXGK_SEGMENTDESCRIPTOR3* ap = &seg[1];
            RtlZeroMemory(ap, sizeof *ap);
            ap->BaseAddress.QuadPart = (LONGLONG)kApBase;
            ap->Flags.Aperture = TRUE;
            ap->Flags.CacheCoherent = TRUE;
            ap->Size = (SIZE_T)kApBytes;
            ap->CommitLimit = (SIZE_T)kApBytes;
        }
        RtlZeroMemory(seg, sizeof *seg);
        seg->BaseAddress.QuadPart = (LONGLONG)d->segPhys;    // addresses Windows gives us are what the display can be pointed at
        seg->CpuTranslatedAddress.QuadPart = (LONGLONG)d->segCpu;
        seg->Flags.CpuVisible = TRUE;
        seg->Size = (SIZE_T)d->segBytes;
        seg->CommitLimit = (SIZE_T)d->segBytes;
        out->PagingBufferSegmentId = d->pagingSegment;
        out->PagingBufferSize = PAGE_SIZE;
        out->PagingBufferPrivateDataSize = sizeof(Op) * 32;
        LogF("segment v3 asked");
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_QUERYSEGMENT4: {
        if (q->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT4)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_QUERYSEGMENTOUT4* out = (DXGK_QUERYSEGMENTOUT4*)q->pOutputData;
        if (!out->pSegmentDescriptor) {
            out->NbSegment = gGdiAccel ? 2 : 1;
            out->SegmentDescriptorStride = sizeof(DXGK_SEGMENTDESCRIPTOR4);
            return STATUS_SUCCESS;
        }
        DXGK_SEGMENTDESCRIPTOR4* seg = (DXGK_SEGMENTDESCRIPTOR4*)out->pSegmentDescriptor;
        if (gGdiAccel && out->NbSegment >= 2) {
            DXGK_SEGMENTDESCRIPTOR4* ap = (DXGK_SEGMENTDESCRIPTOR4*)((UCHAR*)out->pSegmentDescriptor + out->SegmentDescriptorStride);
            RtlZeroMemory(ap, sizeof *ap);
            ap->BaseAddress.QuadPart = (LONGLONG)kApBase;
            ap->Flags.Aperture = TRUE;
            ap->Flags.CacheCoherent = TRUE;
            ap->Size = (SIZE_T)kApBytes;
            ap->CommitLimit = (SIZE_T)kApBytes;
        }
        RtlZeroMemory(seg, sizeof *seg);
        seg->BaseAddress.QuadPart = (LONGLONG)d->segPhys;    // addresses Windows gives us are what the display can be pointed at
        seg->CpuTranslatedAddress.QuadPart = (LONGLONG)d->segCpu;
        seg->Flags.CpuVisible = TRUE;
        seg->Size = (SIZE_T)d->segBytes;
        seg->CommitLimit = (SIZE_T)d->segBytes;
        out->PagingBufferSegmentId = d->pagingSegment;
        out->PagingBufferSize = PAGE_SIZE;
        out->PagingBufferPrivateDataSize = sizeof(Op) * 32;
        LogF("segment v4 asked (stride %u)", (ULONG)out->SegmentDescriptorStride);
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_HISTORYBUFFERPRECISION: {
        // One entry per engine: how many bits of a history-buffer time stamp are valid. We keep no history buffers.
        UINT n = q->OutputDataSize / sizeof(DXGKARG_HISTORYBUFFERPRECISION);
        DXGKARG_HISTORYBUFFERPRECISION* hp = (DXGKARG_HISTORYBUFFERPRECISION*)q->pOutputData;
        for (UINT i = 0; i < n; ++i) hp[i].PrecisionBits = 0;
        LogF("history precision asked (%u entries)", n);
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION: {
        RtlZeroMemory(q->pOutputData, q->OutputDataSize);
        LogF("display caps extension asked (%u bytes)", q->OutputDataSize);
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_PHYSICALADAPTERCAPS: {
        // Windows gives exactly the bytes up to the end of Flags (20), not the padded structure (24).
        const ULONG need = FIELD_OFFSET(DXGK_PHYSICALADAPTERCAPS, Flags) + sizeof(DXGK_PHYSICALADAPTERFLAGS);
        if (q->OutputDataSize < need) { LogF("physical caps: %u bytes given, %u wanted", q->OutputDataSize, need); return STATUS_BUFFER_TOO_SMALL; }
        DXGK_PHYSICALADAPTERCAPS* pc = (DXGK_PHYSICALADAPTERCAPS*)q->pOutputData;
        RtlZeroMemory(pc, q->OutputDataSize < sizeof *pc ? q->OutputDataSize : sizeof *pc);
        pc->NumExecutionNodes = 1;
        pc->PagingNodeIndex = 0;
        LogF("physical caps asked");
        return STATUS_SUCCESS;
    }
    default:
        LogF("adapter info type %u: not supported", (ULONG)q->Type);
        return STATUS_NOT_SUPPORTED;
    }
}

// ---- devices and contexts (one per program and its rendering thread; nothing to keep) ----
static NTSTATUS APIENTRY DdiCreateDevice(IN_CONST_HANDLE context, INOUT_PDXGKARG_CREATEDEVICE arg)
{
    Tr tr("DdiCreateDevice");
    Device* d = (Device*)context;
    arg->hDevice = d;                                    // every device is the adapter
    InterlockedIncrement(&d->devices);
    LogF("device created (%s)", arg->Flags.SystemDevice ? "system" : "program");
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY DdiDestroyDevice(IN_CONST_HANDLE context)
{
    Tr tr("DdiDestroyDevice");
    InterlockedDecrement(&((Device*)context)->devices);
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY DdiCreateContext(IN_CONST_HANDLE device, INOUT_PDXGKARG_CREATECONTEXT arg)
{
    Tr tr("DdiCreateContext");
    Device* d = (Device*)device;
    arg->hContext = d;
    arg->ContextInfo.DmaBufferSegmentSet = 0;
    arg->ContextInfo.DmaBufferSize = 64 * 1024;
    arg->ContextInfo.DmaBufferPrivateDataSize = sizeof(Op);
    arg->ContextInfo.AllocationListSize = DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT;
    arg->ContextInfo.PatchLocationListSize = DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT;
    InterlockedIncrement(&d->contexts);
    LogF("context created (node %u%s)", arg->NodeOrdinal, arg->Flags.SystemContext ? ", system" : "");
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY DdiDestroyContext(IN_CONST_HANDLE context)
{
    Tr tr("DdiDestroyContext");
    InterlockedDecrement(&((Device*)context)->contexts);
    return STATUS_SUCCESS;
}

// ---- allocations ----
static NTSTATUS APIENTRY DdiGetStandardAllocationDriverData(IN_CONST_HANDLE, INOUT_PDXGKARG_GETSTANDARDALLOCATIONDRIVERDATA arg)
{
    Tr tr("DdiGetStandardAllocationDriverData");
    if (!arg->pAllocationPrivateDriverData) {
        arg->AllocationPrivateDriverDataSize = sizeof(P5AllocData);
        arg->ResourcePrivateDriverDataSize = 0;
        return STATUS_SUCCESS;
    }
    if (arg->AllocationPrivateDriverDataSize < sizeof(P5AllocData)) return STATUS_BUFFER_TOO_SMALL;
    P5AllocData* a = (P5AllocData*)arg->pAllocationPrivateDriverData;
    RtlZeroMemory(a, sizeof *a);
    a->magic = P5A_MAGIC;
    switch (arg->StandardAllocationType) {
    case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE: {
        D3DKMDT_SHAREDPRIMARYSURFACEDATA* p = arg->pCreateSharedPrimarySurfaceData;
        a->kind = P5A_PRIMARY;
        a->width = p->Width;
        a->height = p->Height;
        a->format = (ULONG)p->Format;
        break;
    }
    case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE: {
        D3DKMDT_SHADOWSURFACEDATA* p = arg->pCreateShadowSurfaceData;
        a->kind = P5A_SHADOW;
        a->width = p->Width;
        a->height = p->Height;
        a->format = (ULONG)p->Format;
        p->Pitch = p->Width * 4;
        break;
    }
    case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE: {
        D3DKMDT_STAGINGSURFACEDATA* p = arg->pCreateStagingSurfaceData;
        a->kind = P5A_STAGING;
        a->width = p->Width;
        a->height = p->Height;
        a->format = (ULONG)D3DDDIFMT_X8R8G8B8;
        p->Pitch = p->Width * 4;
        break;
    }
    case D3DKMDT_STANDARDALLOCATION_GDISURFACE: {
        D3DKMDT_GDISURFACEDATA* p = arg->pCreateGdiSurfaceData;
        static volatile LONG said;
        if (gGdiAccel) {
            ULONG t = (ULONG)p->Type;
            BOOLEAN ok = p->Width && p->Height && p->Width <= 16384 && p->Height <= 16384 && t >= 1 && t <= 7 && t != 6;
            ULONG bpp = BppOf((ULONG)p->Format);
            if (InterlockedIncrement(&said) <= 60)
                LogF("GDI surface type %u %ux%u format %u (%u bytes/pixel) flags %X: %s", t, p->Width, p->Height, (ULONG)p->Format, bpp, p->Flags.Value, ok ? "ok" : "refused");
            if (!ok) return STATUS_NOT_SUPPORTED;
            a->kind = P5A_GDI | (t << 8);
            a->width = p->Width;
            a->height = p->Height;
            a->format = (ULONG)p->Format;
            a->pitch = (p->Width * bpp + 3) & ~3u;
            a->bytes = (ULONGLONG)a->pitch * a->height;
            p->Pitch = a->pitch;
            return STATUS_SUCCESS;
        }
        BOOLEAN ok = gGdi && p->Type != D3DKMDT_GDISURFACE_EXISTINGSYSMEM && p->Width && p->Height;
        if (InterlockedIncrement(&said) <= 40)
            LogF("GDI surface type %u %ux%u format %u flags %X: %s", (ULONG)p->Type, p->Width, p->Height, (ULONG)p->Format, p->Flags.Value, ok ? "ok" : "refused");
        if (!ok) return STATUS_NOT_SUPPORTED;
        a->kind = P5A_GDI;
        a->width = p->Width;
        a->height = p->Height;
        a->format = (ULONG)p->Format;
        a->gpuPhys = 0;
        p->Pitch = p->Width * 4;
        break;
    }
    default:
        LogF("standard allocation type %u: not supported", (ULONG)arg->StandardAllocationType);
        return STATUS_NOT_SUPPORTED;
    }
    a->pitch = a->width * 4;
    a->bytes = (ULONGLONG)a->pitch * a->height;
    LogF("standard allocation kind %u: %ux%u format %u", a->kind, a->width, a->height, a->format);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY DdiCreateAllocation(IN_CONST_HANDLE context, INOUT_PDXGKARG_CREATEALLOCATION arg)
{
    Tr tr("DdiCreateAllocation");
    Device* d = (Device*)context;
    {
        static volatile LONG said;
        const P5AllocData* first = arg->NumAllocations && arg->pAllocationInfo ? (const P5AllocData*)arg->pAllocationInfo[0].pPrivateDriverData : NULL;
        if (InterlockedIncrement(&said) <= 400)
            TraceF("create allocation: %u allocations, flags %X, resource data %u bytes, first kind %X %ux%u", arg->NumAllocations, arg->Flags.Value, arg->PrivateDriverDataSize,
                   first && arg->pAllocationInfo[0].PrivateDriverDataSize >= sizeof(P5AllocData) ? first->kind : 0xFFFF, first ? first->width : 0, first ? first->height : 0);
    }
    for (UINT i = 0; i < arg->NumAllocations; ++i) {
        DXGK_ALLOCATIONINFO* ai = &arg->pAllocationInfo[i];
        const P5AllocData* in = (const P5AllocData*)ai->pPrivateDriverData;
        if (!in || ai->PrivateDriverDataSize < sizeof(P5AllocData) || in->magic != P5A_MAGIC || in->width == 0 || in->height == 0 || in->width > 16384 ||
            in->height > 16384 || in->pitch < in->width * ((in->kind & 0xFF) == P5A_GDI ? 1 : 4) || in->bytes < (ULONGLONG)in->pitch * in->height || in->bytes > d->segBytes / 2) {
            LogF("allocation %u refused (data %p, %u bytes)", i, in, ai->PrivateDriverDataSize);
            for (UINT k = 0; k < i; ++k) {
                ExFreePoolWithTag(arg->pAllocationInfo[k].hAllocation, P5_TAG);
                InterlockedDecrement(&d->allocs);
            }
            return STATUS_INVALID_PARAMETER;
        }
        Alloc* a = (Alloc*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(Alloc), P5_TAG);
        if (!a) return STATUS_NO_MEMORY;
        a->info = *in;
        ai->hAllocation = a;
        if ((in->kind & 0xFF) == P5A_GDI) TraceF("gdi alloc %p type %u %ux%u created", a, in->kind >> 8, in->width, in->height);
        ai->Alignment = 0x10000;
        ai->Size = (SIZE_T)((in->bytes + PAGE_SIZE - 1) & ~(ULONGLONG)(PAGE_SIZE - 1));
        ai->PitchAlignedSize = 0;
        ai->HintedBank.Value = 0;
        ai->PreferredSegment.Value = 0;
        ai->PreferredSegment.SegmentId0 = 1;
        ai->PreferredSegment.Direction0 = 0;
        ai->SupportedReadSegmentSet = 1;
        ai->SupportedWriteSegmentSet = 1;
        ai->EvictionSegmentSet = RegGet(L"EvictionSet", 0);     // only aperture segments may be named here, and we have none
        ai->MaximumRenamingListLength = 0;
        ai->pAllocationUsageHint = NULL;
        ai->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
        ai->Flags.Value = 0;
        ai->Flags.CpuVisible = TRUE;
        ULONG gt = (in->kind & 0xFF) == P5A_GDI ? in->kind >> 8 : 0;
        if (gt == D3DKMDT_GDISURFACE_STAGING_CPUVISIBLE || gt == D3DKMDT_GDISURFACE_EXISTINGSYSMEM) {
            // GDI's CPU-side surfaces: system memory in the cache-coherent aperture segment (Microsoft's rule for these).
            ai->Alignment = 0;
            ai->PreferredSegment.SegmentId0 = 2;
            ai->SupportedReadSegmentSet = 2;
            ai->SupportedWriteSegmentSet = 2;
            ai->EvictionSegmentSet = 0;
            ai->Flags.Cached = TRUE;
        }
        if ((gt == D3DKMDT_GDISURFACE_TEXTURE || gt == D3DKMDT_GDISURFACE_TEXTURE_CROSSADAPTER) && !gGdiTexCpu) ai->Flags.CpuVisible = FALSE;
        // Shared surfaces of our Direct3D driver (kind 4 with bit 0x100: flip-model buffers and the like): Windows drops a
        // shared allocation that is CPU-visible right after creating it (seen 2026-11-21, as with GDI textures).
        if ((in->kind & 0xFF) == P5A_USER && (in->kind & 0x100)) ai->Flags.CpuVisible = FALSE;
        // History (the aperture design of 2026-11-20, build/ps5gpu/wddm/ps5wddm-0.3.2146-aperture.cpp): there Windows
        // made the pages write-combined and our reads crawled; Flags.Cached made Windows destroy the primary at once.
        InterlockedIncrement(&d->allocs);
    }
    if (arg->Flags.Resource && !arg->hResource) {
        Resource* r = (Resource*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(Resource), P5_TAG);
        if (r) r->magic = P5A_MAGIC;
        arg->hResource = r;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY DdiDestroyAllocation(IN_CONST_HANDLE context, IN_CONST_PDXGKARG_DESTROYALLOCATION arg)
{
    Tr tr("DdiDestroyAllocation");
    Device* d = (Device*)context;
    for (UINT i = 0; i < arg->NumAllocations; ++i) {
        Alloc* a = (Alloc*)arg->pAllocationList[i];
        if (!a) continue;
        if (KindOf(a) == P5A_GDI) TraceF("gdi alloc %p type %u %ux%u destroyed", a, GdiTypeOf(a), a->info.width, a->info.height);
        UnmapAp(a);
        if (d->source == a) {                                // the picture on the screen goes away: show the frame buffer
            d->source = NULL;
            d->sourceAddress.QuadPart = 0;
            ShowFrameBuffer(d);
        }
        ExFreePoolWithTag(a, P5_TAG);
        InterlockedDecrement(&d->allocs);
    }
    if (arg->Flags.DestroyResource && arg->hResource) ExFreePoolWithTag(arg->hResource, P5_TAG);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY DdiDescribeAllocation(IN_CONST_HANDLE adapter, INOUT_PDXGKARG_DESCRIBEALLOCATION arg)
{
    Tr tr("DdiDescribeAllocation");
    Alloc* a = (Alloc*)arg->hAllocation;
    arg->Width = a->info.width;
    arg->Height = a->info.height;
    arg->Format = (D3DDDIFORMAT)a->info.format;
    arg->MultisampleMethod.NumSamples = 0;
    arg->MultisampleMethod.NumQualityLevels = 0;
    Device* dd = (Device*)adapter;
    arg->RefreshRate.Numerator = dd && dd->refreshNum ? dd->refreshNum : 60;
    arg->RefreshRate.Denominator = dd && dd->refreshDen ? dd->refreshDen : 1;
    arg->PrivateDriverFormatAttribute = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY DdiOpenAllocation(IN_CONST_HANDLE device, IN_CONST_PDXGKARG_OPENALLOCATION arg)
{
    Tr tr("DdiOpenAllocation");
    Device* d = (Device*)device;
    for (UINT i = 0; i < arg->NumAllocations; ++i) {
        // Windows' handle -> the Alloc we gave at creation; the "device specific" handle is that same thing.
        DXGKARGCB_GETHANDLEDATA get;
        RtlZeroMemory(&get, sizeof get);
        get.hObject = arg->pOpenAllocation[i].hAllocation;
        get.Type = DXGK_HANDLE_ALLOCATION;
        arg->pOpenAllocation[i].hDeviceSpecificAllocation = d->dxgk.DxgkCbGetHandleData(&get);
    }
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY DdiCloseAllocation(IN_CONST_HANDLE, IN_CONST_PDXGKARG_CLOSEALLOCATION) { Tr tr("DdiCloseAllocation"); return STATUS_SUCCESS; }

// Windows' memory manager moves allocations into the segment, out of it and around in it. Each request becomes one
// operation in the paging buffer's private data, done by the engine thread when the buffer is submitted, in order
// with everything else (the page list of the system-memory side stays valid until that buffer has completed).
static NTSTATUS APIENTRY DdiBuildPagingBuffer(IN_CONST_HANDLE context, IN_PDXGKARG_BUILDPAGINGBUFFER arg)
{
    Tr tr("DdiBuildPagingBuffer");
    Device* d = (Device*)context;
    static volatile LONG said;
    Op op;
    RtlZeroMemory(&op, sizeof op);
    op.magic = OP_MAGIC;
    switch (arg->Operation) {
    case DXGK_OPERATION_TRANSFER: {
        BOOLEAN srcSeg = arg->Transfer.Source.SegmentId != 0, dstSeg = arg->Transfer.Destination.SegmentId != 0;
        op.bytes = arg->Transfer.TransferSize;
        op.mdlOffset = arg->Transfer.MdlOffset;
        op.srcSeg = arg->Transfer.Source.SegmentId;
        op.dstSeg = arg->Transfer.Destination.SegmentId;
        op.src = op.dst = (Alloc*)arg->Transfer.hAllocation;
        if (srcSeg) op.srcAddr = (ULONGLONG)arg->Transfer.Source.SegmentAddress.QuadPart + arg->Transfer.TransferOffset;
        else op.mdl = arg->Transfer.Source.pMdl;
        if (dstSeg) op.dstAddr = (ULONGLONG)arg->Transfer.Destination.SegmentAddress.QuadPart + arg->Transfer.TransferOffset;
        else op.mdl = arg->Transfer.Destination.pMdl;
        op.kind = srcSeg && dstSeg ? OP_PAGE_MOVE : dstSeg ? OP_PAGE_IN : srcSeg ? OP_PAGE_OUT : OP_NOP;
        if (InterlockedIncrement(&said) <= 24)
            LogF("transfer %s: %I64u bytes, segment address %I64X -> %I64X, offset %u, page %u of the list, flags %X",
                 op.kind == OP_PAGE_MOVE ? "move" : op.kind == OP_PAGE_IN ? "in" : op.kind == OP_PAGE_OUT ? "out" : "?", op.bytes, op.srcAddr, op.dstAddr,
                 arg->Transfer.TransferOffset, arg->Transfer.MdlOffset, (ULONG)arg->Transfer.Flags.Value);
        break;
    }
    case DXGK_OPERATION_FILL:
        op.kind = OP_PAGE_FILL;
        op.bytes = arg->Fill.FillSize;
        op.color = arg->Fill.FillPattern;
        op.dstAddr = (ULONGLONG)arg->Fill.Destination.SegmentAddress.QuadPart;
        op.dstSeg = arg->Fill.Destination.SegmentId;
        op.dst = (Alloc*)arg->Fill.hAllocation;
        if (InterlockedIncrement(&said) <= 24) LogF("fill: %I64u bytes at %I64X with %08X", op.bytes, op.dstAddr, op.color);
        break;
    case DXGK_OPERATION_MAP_APERTURE_SEGMENT:                // done by the engine thread, in order with the rest
        op.kind = OP_AP_MAP;
        op.dst = (Alloc*)arg->MapApertureSegment.hAllocation;
        op.dstAddr = kApBase + (ULONGLONG)arg->MapApertureSegment.OffsetInPages * PAGE_SIZE;
        op.dstSeg = arg->MapApertureSegment.SegmentId;
        op.bytes = (ULONGLONG)arg->MapApertureSegment.NumberOfPages * PAGE_SIZE;
        op.mdl = arg->MapApertureSegment.pMdl;
        op.mdlOffset = arg->MapApertureSegment.MdlOffset;
        if (!op.dst || !op.bytes) return STATUS_SUCCESS;
        if (InterlockedIncrement(&said) <= 24) LogF("aperture map: %I64u bytes at %I64X (kind %X)", op.bytes, op.dstAddr, op.dst->info.kind);
        break;
    case DXGK_OPERATION_UNMAP_APERTURE_SEGMENT:
        op.kind = OP_AP_UNMAP;
        op.dst = (Alloc*)arg->UnmapApertureSegment.hAllocation;
        if (!op.dst) return STATUS_SUCCESS;
        break;
    default:                                                 // discard content and the like: nothing to do, never an error
        if (InterlockedIncrement(&said) <= 24) LogF("paging operation %u: nothing to do", (ULONG)arg->Operation);
        return STATUS_SUCCESS;
    }
    if (op.kind == OP_NOP) return STATUS_SUCCESS;
    if (arg->pDmaBuffer && arg->DmaSize >= 16 && arg->pDmaBufferPrivateData && arg->DmaBufferPrivateDataSize >= sizeof(Op)) {
        if (op.mdl) {
            // The engine thread works from our own copy of the page numbers: whether Windows keeps its list structure
            // until the buffer has run is not something to find out the hard way.
            SIZE_T pages = (SIZE_T)((op.bytes + PAGE_SIZE - 1) / PAGE_SIZE);
            const PFN_NUMBER* pfn = MdlPages(op.mdl, op.mdlOffset, pages);
            PFN_NUMBER* own = pfn && pages ? (PFN_NUMBER*)ExAllocatePool2(POOL_FLAG_NON_PAGED, pages * sizeof(PFN_NUMBER), P5_TAG) : NULL;
            if (own) {
                RtlCopyMemory(own, pfn, pages * sizeof(PFN_NUMBER));
                op.ticket = PutTicket(d, own);
                if (!op.ticket) { ExFreePoolWithTag(own, P5_TAG); own = NULL; }
            }
            if (!own) {
                DoPage(d, &op, NULL);                        // no room for a copy (or a bad list: counted as a failure there)
                return STATUS_SUCCESS;
            }
            op.mdl = NULL;
        }
        *(Op*)arg->pDmaBufferPrivateData = op;
        RtlZeroMemory(arg->pDmaBuffer, 16);
        arg->pDmaBuffer = (UCHAR*)arg->pDmaBuffer + 16;
        arg->pDmaBufferPrivateData = (UCHAR*)arg->pDmaBufferPrivateData + sizeof(Op);
        d->pagingInsufficient = 0;
        return STATUS_SUCCESS;
    }
    // No room: Windows submits what there is and asks again with a fresh buffer. Should a fresh one have no room
    // either (the private data is not what we asked for), do it here and now rather than ask for ever.
    if (d->pagingInsufficient++ == 0) return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    LogF("paging buffer without room twice (dma %u, private %u): done directly", arg->DmaSize, arg->DmaBufferPrivateDataSize);
    DoPage(d, &op, NULL);
    return STATUS_SUCCESS;
}

// ---- presenting and rendering: what goes into a DMA buffer ----
static NTSTATUS APIENTRY DdiPresent(IN_CONST_HANDLE context, INOUT_PDXGKARG_PRESENT arg)
{
    Tr tr("DdiPresent");
    Device* d = (Device*)context;
    d->presents3++;
    if (arg->Flags.Flip) {                                   // the flip itself arrives as SetVidPnSourceAddress
        d->flips++;
        return STATUS_SUCCESS;
    }
    if (!arg->pDmaBuffer || !arg->pDmaBufferPrivateData) return STATUS_SUCCESS;          // nothing to write into: nothing to do
    if (arg->DmaSize < 16 || arg->DmaBufferPrivateDataSize < sizeof(Op)) return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    Op* op = (Op*)arg->pDmaBufferPrivateData;
    RtlZeroMemory(op, sizeof *op);
    op->magic = OP_MAGIC;
    DXGK_ALLOCATIONLIST* src = &arg->pAllocationList[DXGK_PRESENT_SOURCE_INDEX];
    DXGK_ALLOCATIONLIST* dst = &arg->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX];
    op->kind = arg->Flags.ColorFill ? OP_FILL : arg->Flags.Blt ? OP_BLT : OP_NOP;
    // Where the two are right now, as far as Windows knows at this point; DxgkDdiPatch corrects it if they move
    // (or only arrive) before the buffer runs.
    if (op->kind == OP_BLT && src->hDeviceSpecificAllocation) {
        op->src = (Alloc*)src->hDeviceSpecificAllocation;
        op->srcSeg = src->SegmentId;
        if (src->SegmentId) op->srcAddr = (ULONGLONG)src->PhysicalAddress.QuadPart;
    }
    if (dst->hDeviceSpecificAllocation) {
        op->dst = (Alloc*)dst->hDeviceSpecificAllocation;
        op->dstSeg = dst->SegmentId;
        if (dst->SegmentId) op->dstAddr = (ULONGLONG)dst->PhysicalAddress.QuadPart;
    }
    op->srcRect = arg->SrcRect;
    op->dstRect = arg->DstRect;
    op->color = arg->Color;
    if (arg->SubRectCnt && arg->SubRectCnt <= 16 && arg->pDstSubRects) {
        op->nrects = arg->SubRectCnt;
        for (UINT i = 0; i < arg->SubRectCnt; ++i) op->rects[i] = arg->pDstSubRects[i];
    }
    {
        static volatile LONG said;
        ULONG sk = op->src ? KindOf(op->src) : 0, dk = op->dst ? KindOf(op->dst) : 0;
        if ((sk == P5A_USER || dk == P5A_GDI || sk == P5A_GDI) && InterlockedIncrement(&said) <= 40)
            TraceF("present flags %X src %p kind %u (%ux%u) dst %p kind %u (%ux%u) %ld,%ld-%ld,%ld rects %u", arg->Flags.Value, op->src, sk, op->src ? op->src->info.width : 0,
                   op->src ? op->src->info.height : 0, op->dst, dk, op->dst ? op->dst->info.width : 0, op->dst ? op->dst->info.height : 0,
                   arg->DstRect.left, arg->DstRect.top, arg->DstRect.right, arg->DstRect.bottom, arg->SubRectCnt);
    }
    if (d->presents3 <= 6)
        LogF("present %s src %p at %I64X dst %p at %I64X %ld,%ld-%ld,%ld rects %u", arg->Flags.ColorFill ? "fill" : arg->Flags.Blt ? "blt" : "other", op->src, op->srcAddr, op->dst,
             op->dstAddr, arg->DstRect.left, arg->DstRect.top, arg->DstRect.right, arg->DstRect.bottom, arg->SubRectCnt);
    // Both allocations must be in the segment when this runs: say so in the patch list. DriverId tells DxgkDdiPatch
    // which of the two an entry is.
    UINT n = 0;
    if (arg->pPatchLocationListOut && arg->PatchLocationListOutSize >= 2) {
        if (op->src) {
            D3DDDI_PATCHLOCATIONLIST* pl = &arg->pPatchLocationListOut[n++];
            RtlZeroMemory(pl, sizeof *pl);
            pl->AllocationIndex = DXGK_PRESENT_SOURCE_INDEX;
            pl->DriverId = 1;
        }
        if (op->dst) {
            D3DDDI_PATCHLOCATIONLIST* pl = &arg->pPatchLocationListOut[n++];
            RtlZeroMemory(pl, sizeof *pl);
            pl->AllocationIndex = DXGK_PRESENT_DESTINATION_INDEX;
            pl->DriverId = 2;
        }
        arg->pPatchLocationListOut += n;
    }
    RtlZeroMemory(arg->pDmaBuffer, 16);
    arg->pDmaBuffer = (UCHAR*)arg->pDmaBuffer + 16;
    arg->pDmaBufferPrivateData = (UCHAR*)arg->pDmaBufferPrivateData + sizeof(Op);
    return STATUS_SUCCESS;
}

// The incoming patch list is the program's memory: it may be gone or bad.
static BOOLEAN CopyPatchList(DXGKARG_RENDER* arg, UINT n)
{
    __try {
        for (UINT i = 0; i < n; ++i) {
            D3DDDI_PATCHLOCATIONLIST* pl = &arg->pPatchLocationListOut[i];
            RtlZeroMemory(pl, sizeof *pl);
            pl->AllocationIndex = arg->pPatchLocationListIn[i].AllocationIndex;
            pl->SlotId = i;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
    return TRUE;
}

static NTSTATUS APIENTRY DdiRender(IN_CONST_HANDLE context, INOUT_PDXGKARG_RENDER arg)
{
    Tr tr("DdiRender");
    Device* d = (Device*)context;
    d->renders++;
    if (!arg->pDmaBuffer || !arg->pDmaBufferPrivateData) return STATUS_SUCCESS;
    if (arg->DmaSize < 16 || arg->DmaBufferPrivateDataSize < sizeof(Op)) return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    Op* op = (Op*)arg->pDmaBufferPrivateData;
    RtlZeroMemory(op, sizeof *op);
    op->magic = OP_MAGIC;
    // Our Direct3D driver's one command: copy allocation [src] into [dst] (a GDI surface the compositor cannot lock,
    // into a staging allocation it can), in order after everything drawn into it so far.
    UINT cmd[7] = {};
    BOOLEAN copy = FALSE, rect = FALSE;
    __try {
        if (arg->pCommand && arg->CommandLength >= 24) {
            RtlCopyMemory(cmd, arg->pCommand, arg->CommandLength >= sizeof cmd ? sizeof cmd : 24);
            rect = cmd[0] == 0x32433550u && arg->CommandLength >= sizeof cmd;      // "P5C2": magic, src, dst, left, top, right, bottom
            copy = rect || cmd[0] == 0x50433550u;                                   // "P5CP": magic, src, dst, width, height (whole surface)
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return STATUS_INVALID_PARAMETER;
    }
    if (copy) {
        if (cmd[1] >= arg->AllocationListSize || cmd[2] >= arg->AllocationListSize || arg->PatchLocationListOutSize < 2) return STATUS_INVALID_PARAMETER;
        const DXGK_ALLOCATIONLIST *s = &arg->pAllocationList[cmd[1]], *t = &arg->pAllocationList[cmd[2]];
        op->kind = OP_BLT;
        op->src = (Alloc*)s->hDeviceSpecificAllocation;
        op->dst = (Alloc*)t->hDeviceSpecificAllocation;
        if (!op->src || !op->dst) return STATUS_INVALID_PARAMETER;
        op->srcSeg = s->SegmentId;
        op->dstSeg = t->SegmentId;
        if (s->SegmentId) op->srcAddr = (ULONGLONG)s->PhysicalAddress.QuadPart;
        if (t->SegmentId) op->dstAddr = (ULONGLONG)t->PhysicalAddress.QuadPart;
        LONG w = (LONG)min(op->src->info.width, op->dst->info.width), h = (LONG)min(op->src->info.height, op->dst->info.height);
        op->srcRect.right = op->dstRect.right = w;
        op->srcRect.bottom = op->dstRect.bottom = h;
        if (rect) {
            if (cmd[3] >= cmd[5] || cmd[4] >= cmd[6] || cmd[5] > (UINT)w || cmd[6] > (UINT)h) return STATUS_INVALID_PARAMETER;
            op->srcRect.left = op->dstRect.left = (LONG)cmd[3];
            op->srcRect.top = op->dstRect.top = (LONG)cmd[4];
            op->srcRect.right = op->dstRect.right = (LONG)cmd[5];
            op->srcRect.bottom = op->dstRect.bottom = (LONG)cmd[6];
        }
        D3DDDI_PATCHLOCATIONLIST* pl = arg->pPatchLocationListOut;
        RtlZeroMemory(pl, 2 * sizeof *pl);
        pl[0].AllocationIndex = cmd[1];
        pl[0].DriverId = 1;
        pl[1].AllocationIndex = cmd[2];
        pl[1].DriverId = 2;
        pl[1].SlotId = 1;
        arg->pPatchLocationListOut += 2;
        static volatile LONG said;
        if (InterlockedIncrement(&said) <= 6) TraceF("copy command: %p (kind %X) -> %p (kind %X) %ldx%ld", op->src, op->src->info.kind, op->dst, op->dst->info.kind, w, h);
        RtlZeroMemory(arg->pDmaBuffer, 16);
        arg->pDmaBuffer = (UCHAR*)arg->pDmaBuffer + 16;
        arg->pDmaBufferPrivateData = (UCHAR*)arg->pDmaBufferPrivateData + sizeof(Op);
        return STATUS_SUCCESS;
    }
    UINT n = arg->PatchLocationListInSize < arg->PatchLocationListOutSize ? arg->PatchLocationListInSize : arg->PatchLocationListOutSize;
    if (!CopyPatchList(arg, n)) return STATUS_INVALID_PARAMETER;
    arg->pPatchLocationListOut += n;
    RtlZeroMemory(arg->pDmaBuffer, 16);
    arg->pDmaBuffer = (UCHAR*)arg->pDmaBuffer + 16;
    arg->pDmaBufferPrivateData = (UCHAR*)arg->pDmaBufferPrivateData + sizeof(Op);
    return STATUS_SUCCESS;
}

static ULONG GdiRects(const DXGK_RENDERKM_COMMAND* c, const RECT** pr)
{
    switch (c->OpCode) {
    case DXGK_GDIOP_BITBLT: *pr = c->Command.BitBlt.pSubRects; return c->Command.BitBlt.NumSubRects;
    case DXGK_GDIOP_COLORFILL: *pr = c->Command.ColorFill.pSubRects; return c->Command.ColorFill.NumSubRects;
    case DXGK_GDIOP_ALPHABLEND: *pr = c->Command.AlphaBlend.pSubRects; return c->Command.AlphaBlend.NumSubRects;
    case DXGK_GDIOP_STRETCHBLT: *pr = c->Command.StretchBlt.pSubRects; return c->Command.StretchBlt.NumSubRects;
    case DXGK_GDIOP_TRANSPARENTBLT: *pr = c->Command.TransparentBlt.pSubRects; return c->Command.TransparentBlt.NumSubRects;
    case DXGK_GDIOP_CLEARTYPEBLEND: *pr = c->Command.ClearTypeBlend.pSubRects; return c->Command.ClearTypeBlend.NumSubRects;
    default: *pr = NULL; return 0;
    }
}

static GdiBlob* PeekBlob(Device* d, ULONG id)
{
    GdiBlob* b = NULL;
    KIRQL irql;
    KeAcquireSpinLock(&d->queueLock, &irql);
    for (ULONG i = 0; id && i < kTickets; ++i)
        if (d->tickets[i].id == id) { b = (GdiBlob*)d->tickets[i].pfns; break; }
    KeReleaseSpinLock(&d->queueLock, irql);
    return b;
}

static NTSTATUS APIENTRY DdiRenderKm(IN_CONST_HANDLE context, INOUT_PDXGKARG_RENDER arg)
{
    Tr tr("DdiRenderKm");
    Device* d = (Device*)context;
    d->renderKms++;
    if (!arg->pDmaBuffer || !arg->pDmaBufferPrivateData || arg->DmaSize < 16 || arg->DmaBufferPrivateDataSize < sizeof(Op)) return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    const UCHAR* p0 = (const UCHAR*)arg->pCommand;
    const SIZE_T len = arg->CommandLength, head = FIELD_OFFSET(DXGK_RENDERKM_COMMAND, Command);
    SIZE_T need = sizeof(GdiBlob);
    ULONG ncmd = 0;
    GdiBlob* b = NULL;
    __try {
        for (SIZE_T at = 0; p0 && at + head <= len;) {
            const DXGK_RENDERKM_COMMAND* c = (const DXGK_RENDERKM_COMMAND*)(p0 + at);
            if (c->CommandSize < head || c->CommandSize > len - at) break;
            const RECT* r;
            ULONG nr = GdiRects(c, &r);
            if (!r) nr = 0;
            if (nr > kGdiMaxRects) nr = kGdiMaxRects;
            need += sizeof(GdiCmd) + (SIZE_T)nr * sizeof(RECT);
            ++ncmd;
            at += c->CommandSize;
        }
        if (!ncmd) return STATUS_SUCCESS;
        b = (GdiBlob*)ExAllocatePool2(POOL_FLAG_NON_PAGED, need, P5_TAG);
        if (!b) return STATUS_NO_MEMORY;
        b->bytes = need;
        b->ncmd = ncmd;
        b->nal = arg->AllocationListSize < kGdiAl ? arg->AllocationListSize : kGdiAl;
        for (ULONG i = 0; i < b->nal && arg->pAllocationList; ++i) {
            const DXGK_ALLOCATIONLIST* al = &arg->pAllocationList[i];
            b->al[i].a = (Alloc*)al->hDeviceSpecificAllocation;
            b->al[i].seg = al->SegmentId;
            b->al[i].addr = al->SegmentId ? (ULONGLONG)al->PhysicalAddress.QuadPart : 0;
        }
        UCHAR* out = (UCHAR*)(b + 1);
        for (SIZE_T at = 0, k = 0; k < ncmd; ++k) {
            const DXGK_RENDERKM_COMMAND* c = (const DXGK_RENDERKM_COMMAND*)(p0 + at);
            const RECT* r;
            ULONG nr = GdiRects(c, &r);
            if (!r) nr = 0;
            if (nr > kGdiMaxRects) { nr = kGdiMaxRects; GdiSay(d, 0, "sub-rectangles cut to 4096"); }
            GdiCmd* g = (GdiCmd*)out;
            RtlZeroMemory(g, sizeof *g);
            RtlCopyMemory(&g->c, c, c->CommandSize < sizeof g->c ? c->CommandSize : sizeof g->c);
            g->nrects = nr;
            g->size = (ULONG)(sizeof(GdiCmd) + (SIZE_T)nr * sizeof(RECT));
            if (nr) RtlCopyMemory(g + 1, r, (SIZE_T)nr * sizeof(RECT));
            out += g->size;
            at += c->CommandSize;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (b) ExFreePoolWithTag(b, P5_TAG);
        GdiSay(d, 0, "command buffer not readable");
        return STATUS_INVALID_PARAMETER;
    }
    ULONG ticket = PutTicket(d, (PFN_NUMBER*)b);
    if (!ticket) {                                           // no room: these commands are lost (counted), nothing worse
        ExFreePoolWithTag(b, P5_TAG);
        d->gdiNoTicket++;
        return STATUS_SUCCESS;
    }
    Op* op = (Op*)arg->pDmaBufferPrivateData;
    RtlZeroMemory(op, sizeof *op);
    op->magic = OP_MAGIC;
    op->kind = OP_GDI;
    op->ticket = ticket;
    UINT n = 0;
    for (UINT i = 0; i < b->nal && n < arg->PatchLocationListOutSize && arg->pPatchLocationListOut; ++i) {
        D3DDDI_PATCHLOCATIONLIST* pl = &arg->pPatchLocationListOut[n];
        RtlZeroMemory(pl, sizeof *pl);
        pl->AllocationIndex = i;
        pl->DriverId = 3;
        pl->SlotId = n++;
    }
    arg->pPatchLocationListOut += n;
    RtlZeroMemory(arg->pDmaBuffer, 16);
    arg->pDmaBuffer = (UCHAR*)arg->pDmaBuffer + 16;
    arg->pDmaBufferPrivateData = (UCHAR*)arg->pDmaBufferPrivateData + sizeof(Op);
    static volatile LONG said;
    if (InterlockedIncrement(&said) <= 8) TraceF("renderkm: %u commands, %u allocations, first op %u, %Iu bytes", ncmd, b->nal, (ULONG)((GdiCmd*)(b + 1))->c.OpCode, need);
    return STATUS_SUCCESS;
}

// Where a buffer's allocations really are, just before it runs: into the operation it carries.
static NTSTATUS APIENTRY DdiPatch(IN_CONST_HANDLE context, IN_CONST_PDXGKARG_PATCH arg)
{
    Tr tr("DdiPatch");
    Device* d = (Device*)context;
    if (arg->Flags.Paging || !arg->pDmaBufferPrivateData || !arg->pPatchLocationList || !arg->pAllocationList) return STATUS_SUCCESS;
    ULONG at = arg->DmaBufferPrivateDataSubmissionStartOffset;
    if (arg->DmaBufferPrivateDataSize < sizeof(Op) || at > arg->DmaBufferPrivateDataSize - sizeof(Op)) return STATUS_SUCCESS;
    Op* op = (Op*)((UCHAR*)arg->pDmaBufferPrivateData + at);
    if (op->magic != OP_MAGIC) return STATUS_SUCCESS;
    if (op->kind == OP_GDI) {                                // every operation of this submission: allocation list as it is now
        ULONG end = arg->DmaBufferPrivateDataSubmissionEndOffset;
        if (end > arg->DmaBufferPrivateDataSize) end = arg->DmaBufferPrivateDataSize;
        for (; at + sizeof(Op) <= end; at += sizeof(Op)) {
            Op* g = (Op*)((UCHAR*)arg->pDmaBufferPrivateData + at);
            if (g->magic != OP_MAGIC || g->kind != OP_GDI) continue;
            GdiBlob* b = PeekBlob(d, g->ticket);
            if (!b) continue;
            for (ULONG i = 0; i < b->nal && i < arg->AllocationListSize; ++i) {
                const DXGK_ALLOCATIONLIST* al = &arg->pAllocationList[i];
                b->al[i].seg = al->SegmentId;
                b->al[i].addr = al->SegmentId ? (ULONGLONG)al->PhysicalAddress.QuadPart : 0;
            }
        }
        return STATUS_SUCCESS;
    }
    for (UINT i = 0; i < arg->PatchLocationListSubmissionLength; ++i) {
        const D3DDDI_PATCHLOCATIONLIST* pl = &arg->pPatchLocationList[arg->PatchLocationListSubmissionStart + i];
        if (pl->AllocationIndex >= arg->AllocationListSize) continue;
        const DXGK_ALLOCATIONLIST* al = &arg->pAllocationList[pl->AllocationIndex];
        ULONGLONG addr = al->SegmentId ? (ULONGLONG)al->PhysicalAddress.QuadPart : 0;
        if (pl->DriverId == 1) { op->srcAddr = addr; op->srcSeg = al->SegmentId; }
        else if (pl->DriverId == 2) { op->dstAddr = addr; op->dstSeg = al->SegmentId; }
    }
    if (d->patches++ < 6) LogF("patch: src at %I64X dst at %I64X (private data offset %u)", op->srcAddr, op->dstAddr, at);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY DdiSubmitCommand(IN_CONST_HANDLE context, IN_CONST_PDXGKARG_SUBMITCOMMAND arg)
{
    Tr tr("DdiSubmitCommand");
    Device* d = (Device*)context;
    InterlockedExchange(&d->lastSubmitted, (LONG)arg->SubmissionFenceId);
    // The operations of this submission: the part of the private data Windows names (buffers without any of ours,
    // or with none at all, simply complete).
    ULONG s = arg->DmaBufferPrivateDataSubmissionStartOffset, e = arg->DmaBufferPrivateDataSubmissionEndOffset, n = 0;
    if (arg->pDmaBufferPrivateData && e > s && e <= arg->DmaBufferPrivateDataSize) n = (e - s) / sizeof(Op);
    if (d->submits++ < 10)
        TraceF("submit fence %u: private data %u..%u of %u -> %u operations%s", arg->SubmissionFenceId, s, e, arg->DmaBufferPrivateDataSize, n, arg->Flags.Paging ? " (paging)" : "");
    EnqueueOps(d, arg->SubmissionFenceId, n ? (const Op*)((const UCHAR*)arg->pDmaBufferPrivateData + s) : NULL, n, OP_NOP);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY DdiPreemptCommand(IN_CONST_HANDLE context, IN_CONST_PDXGKARG_PREEMPTCOMMAND arg)
{
    Tr tr("DdiPreemptCommand");
    EnqueueOps((Device*)context, arg->PreemptionFenceId, NULL, 0, OP_PREEMPT);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY DdiQueryCurrentFence(IN_CONST_HANDLE context, INOUT_PDXGKARG_QUERYCURRENTFENCE arg)
{
    Tr tr("DdiQueryCurrentFence");
    arg->CurrentFence = (ULONG)((Device*)context)->lastCompleted;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY DdiResetFromTimeout(IN_CONST_HANDLE context)
{
    Tr tr("DdiResetFromTimeout");
    Device* d = (Device*)context;
    KIRQL irql;
    KeAcquireSpinLock(&d->queueLock, &irql);
    DiscardQueue(d);                                         // whatever was queued is forgotten, as a reset means
    KeReleaseSpinLock(&d->queueLock, irql);
    LogF("RESET FROM TIMEOUT (submitted %ld completed %ld)", d->lastSubmitted, d->lastCompleted);
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY DdiRestartFromTimeout(IN_CONST_HANDLE) { Tr tr("DdiRestartFromTimeout"); return STATUS_SUCCESS; }
static NTSTATUS APIENTRY DdiCollectDbgInfo(IN_CONST_HANDLE, IN_CONST_PDXGKARG_COLLECTDBGINFO) { Tr tr("DdiCollectDbgInfo"); return STATUS_SUCCESS; }
static NTSTATUS APIENTRY DdiQueryEngineStatus(IN_CONST_HANDLE, INOUT_PDXGKARG_QUERYENGINESTATUS arg)
{
    Tr tr("DdiQueryEngineStatus");
    arg->EngineStatus.Responsive = 1;
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY DdiResetEngine(IN_CONST_HANDLE context, INOUT_PDXGKARG_RESETENGINE arg)
{
    Tr tr("DdiResetEngine");
    arg->LastAbortedFenceId = (ULONG)((Device*)context)->lastCompleted;
    LogF("RESET ENGINE");
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY DdiCancelCommand(IN_CONST_HANDLE, IN_CONST_PDXGKARG_CANCELCOMMAND) { Tr tr("DdiCancelCommand"); return STATUS_SUCCESS; }
static NTSTATUS APIENTRY DdiGetNodeMetadata(IN_CONST_HANDLE, UINT, OUT_PDXGKARG_GETNODEMETADATA arg)
{
    Tr tr("DdiGetNodeMetadata");
    RtlZeroMemory(arg, sizeof *arg);
    arg->EngineType = DXGK_ENGINE_TYPE_3D;
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY DdiEscape(IN_CONST_HANDLE, IN_CONST_PDXGKARG_ESCAPE) { Tr tr("DdiEscape"); return STATUS_NOT_SUPPORTED; }

static NTSTATUS APIENTRY DdiControlInterrupt(IN_CONST_HANDLE context, IN_CONST_DXGK_INTERRUPT_TYPE type, IN_BOOLEAN enable)
{
    Tr tr("DdiControlInterrupt");
    Device* d = (Device*)context;
    if (type == DXGK_INTERRUPT_CRTC_VSYNC) InterlockedExchange(&d->vsyncOn, enable ? 1 : 0);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY DdiGetScanLine(IN_CONST_HANDLE, INOUT_PDXGKARG_GETSCANLINE arg)
{
    Tr tr("DdiGetScanLine");
    arg->InVerticalBlank = FALSE;
    arg->ScanLine = 0;
    return STATUS_SUCCESS;
}

// The flip: Windows names the allocation that is to be on the screen from the next vertical sync on.
static NTSTATUS APIENTRY DdiSetVidPnSourceAddress(IN_CONST_HANDLE context, IN_CONST_PDXGKARG_SETVIDPNSOURCEADDRESS arg)
{
    Tr tr("DdiSetVidPnSourceAddress");
    Device* d = (Device*)context;
    d->source = (Alloc*)arg->hAllocation;
    d->sourceAddress = arg->PrimaryAddress;
    InterlockedExchange(&d->sourceChanged, 1);
    // May be called at a raised interrupt level: nothing here but the flip's register writes.
    if (d->visible && arg->PrimarySegment == 1 && SegPtr(d, (ULONGLONG)arg->PrimaryAddress.QuadPart, d->fbBytes)) FlipTo(d, (ULONGLONG)arg->PrimaryAddress.QuadPart);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY FullSetVidPnSourceVisibility(IN_CONST_HANDLE context, IN_CONST_PDXGKARG_SETVIDPNSOURCEVISIBILITY arg)
{
    Tr tr("FullSetVidPnSourceVisibility");
    Device* d = (Device*)context;
    InterlockedExchange(&d->visible, arg->Visible ? 1 : 0);
    if (!arg->Visible) {
        BlackOut(d);
        ShowFrameBuffer(d);
    } else if (d->source && SegPtr(d, (ULONGLONG)d->sourceAddress.QuadPart, d->fbBytes)) {
        FlipTo(d, (ULONGLONG)d->sourceAddress.QuadPart);
    }
    LogF("visible %u (source at %I64X)", (ULONG)arg->Visible, (ULONGLONG)d->sourceAddress.QuadPart);
    return STATUS_SUCCESS;
}

// Entry points Windows may insist on although nothing here needs them.
static NTSTATUS APIENTRY DdiRecommendVidPnTopology(IN_CONST_HANDLE, IN_CONST_PDXGKARG_RECOMMENDVIDPNTOPOLOGY_CONST) { Tr tr("DdiRecommendVidPnTopology"); return STATUS_GRAPHICS_NO_RECOMMENDED_VIDPN_TOPOLOGY; }
static NTSTATUS APIENTRY DdiStopCapture(IN_CONST_HANDLE, IN_CONST_PDXGKARG_STOPCAPTURE) { Tr tr("DdiStopCapture"); return STATUS_SUCCESS; }
static NTSTATUS APIENTRY DdiSetPalette(IN_CONST_HANDLE, IN_CONST_PDXGKARG_SETPALETTE) { Tr tr("DdiSetPalette"); return STATUS_SUCCESS; }
static NTSTATUS APIENTRY DdiAcquireSwizzlingRange(IN_CONST_HANDLE, INOUT_PDXGKARG_ACQUIRESWIZZLINGRANGE) { Tr tr("DdiAcquireSwizzlingRange"); return STATUS_NOT_SUPPORTED; }
static NTSTATUS APIENTRY DdiReleaseSwizzlingRange(IN_CONST_HANDLE, IN_CONST_PDXGKARG_RELEASESWIZZLINGRANGE) { Tr tr("DdiReleaseSwizzlingRange"); return STATUS_NOT_SUPPORTED; }
static NTSTATUS APIENTRY DdiQueryDependentEngineGroup(IN_CONST_HANDLE, INOUT_DXGKARG_QUERYDEPENDENTENGINEGROUP arg)
{
    Tr tr("DdiQueryDependentEngineGroup");
    arg->DependentNodeOrdinalMask = 1ull << arg->NodeOrdinal;        // one engine, depending on nothing else
    return STATUS_SUCCESS;
}

static VOID APIENTRY FullDpcRoutine(IN_CONST_PVOID context)
{
    Device* d = (Device*)context;
    DrainInterruptRing(d);
    if (InterlockedExchange(&d->notifyPending, 0)) d->dxgk.DxgkCbNotifyDpc(d->dxgk.DeviceHandle);
}

// =====================================================================================================================
// GPU start at boot (2026-11-22)
// =====================================================================================================================
//
// Before: ~30 s after boot the SYSTEM task ran autostart.cmd -> start.cmd -> ps5gpu2 run (the GPU start), then
// wddm-mode.cmd full restarted the display device to bring this driver up in full mode: one visible blink. Now
// DriverEntry runs the same start itself (gpuboot.cpp: stage2's steps 1-6 and 8, register write for register write
// the same as stage2 on the model, build-gpuboot-mock.sh) BEFORE it decides how to register with Windows. Started:
// register as the full driver at once, no device restart. Anything else: register display-only exactly as before
// and leave the reason; autostart.cmd then does what it always did.
//
// It runs only when all of these hold (else display-only, reason in GpuBootReason, steps in GpuBootLog and Log):
//   - first load of the driver in this Windows session: Session\FullDriver absent (wddm-mode.cmd's explicit switches
//     win, so gpu-recover.cmd and the watchdog work as before) and Session\BootTried absent (once per session);
//   - not switched off: service value GpuBootStart is not 0 (the opt-out) and GpuBootOff is not 1 (latched);
//   - at boot: within 120 s of Windows starting. A live test (gpu-bootstart.cmd test) sets the volatile
//     Session\BootTest=1 instead; then programs attached to the GPU (libps5gpu's state) make it stop;
//   - the previous start of this driver finished (StartPending 0);
//   - the PS5's GPU at PCI 20:00.0 with memory decoding on;
//   - C:\ps5\gpu\umd-dwm-on present, autostart.nofull and autostart.off absent (the files autostart.cmd obeys);
//   - the kernel shim ps5gpu.sys installed and set to load at boot (service ps5gpu, Start=1). The compositor's
//     Direct3D driver reaches the GPU through it, so in full mode it has to be loaded before dwm.exe starts;
//   - the latch file C:\ps5\gpu\gpuboot.latch starts with IDLE (below);
//   - the seven microcode files readable from C:\ps5\gpu\fw (or the NT path in the service value GpuBootFwDir).
//
// The latch is a file, not a registry value: this early in the boot Windows does not write the registry to disk yet
// (that begins once smss.exe has initialised it), so a registry marker set now would vanish in the very hang it is
// meant to record. The file is 4096 bytes, rewritten in place with write-through and no caching (as the trace file).
// First word: IDLE = armed; TRYING = written before the start (with the step, rewritten at each step) and kept until
// full mode has run 20 s ("trusted"), then IDLE again; a TRYING found at boot = the last start or its first seconds of
// full mode did not survive: the driver writes OFF (and GpuBootOff=1) and stays display-only until someone runs
// "gpu-bootstart.cmd on". autostart.cmd creates the file (gpu-bootstart.cmd arm) after a good start the old way.
//
// Result values (service key): GpuBootResult 1 started / 2 skipped / 3 failed / 4 latched off; GpuBootReason,
// GpuBootLog (text), GpuBootStep, GpuBootMs, GpuBootUptimeMs, GpuBootTrusted, GpuBootShim (shim seen at trust time);
// GpuBootCapable=1 tells autostart.cmd this driver can do it. Volatile Session\BootStart=1: started this session.

static volatile LONG gBootSettled;       // the latch went back to IDLE after this load's start
static const ULONG kBootWindowMs = 120000, kBootBudgetMs = 5000, kFwMaxBytes = 0x100000;
static WCHAR gPs5Dir[160];               // C:\ps5\gpu as an NT path without a drive letter (BootPaths)
static CHAR gBootLog[3600];
static ULONG gBootLogLen;

struct BootHw {
    volatile ULONG* regs;                // BAR5, mapped only for the start
    ULONG regDwords;
    volatile UCHAR* bar2;                // doorbells
    HANDLE latch;
    ULONG uptimeMs;
};

static void RegSetSz(PCWSTR name, const CHAR* text)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    SIZE_T n = 0;
    if (!NT_SUCCESS(RtlStringCbLengthA(text, 8192, &n))) return;
    WCHAR* wide = (WCHAR*)ExAllocatePool2(POOL_FLAG_PAGED, (n + 1) * sizeof(WCHAR), P5_TAG);
    if (!wide) return;
    for (SIZE_T i = 0; i < n; ++i) wide[i] = (WCHAR)(UCHAR)text[i];
    wide[n] = 0;
    HANDLE key;
    if (NT_SUCCESS(RegOpen(&key))) {
        UNICODE_STRING u;
        RtlInitUnicodeString(&u, name);
        ZwSetValueKey(key, &u, 0, REG_SZ, wide, (ULONG)((n + 1) * sizeof(WCHAR)));
        ZwClose(key);
    }
    ExFreePoolWithTag(wide, P5_TAG);
}

// A REG_SZ from the service key into `out` (empty if absent or not a string).
static void RegGetSz(PCWSTR name, WCHAR* out, ULONG outBytes)
{
    out[0] = 0;
    HANDLE key;
    if (!NT_SUCCESS(RegOpen(&key))) return;
    UNICODE_STRING u;
    RtlInitUnicodeString(&u, name);
    ULONG need = sizeof(KEY_VALUE_PARTIAL_INFORMATION) + outBytes;
    KEY_VALUE_PARTIAL_INFORMATION* info = (KEY_VALUE_PARTIAL_INFORMATION*)ExAllocatePool2(POOL_FLAG_PAGED, need, P5_TAG);
    if (info) {
        ULONG got = 0;
        if (NT_SUCCESS(ZwQueryValueKey(key, &u, KeyValuePartialInformation, info, need, &got)) && (info->Type == REG_SZ || info->Type == REG_EXPAND_SZ) &&
            info->DataLength >= sizeof(WCHAR) && info->DataLength < outBytes) {
            RtlCopyMemory(out, info->Data, info->DataLength);
            out[info->DataLength / sizeof(WCHAR)] = 0;
        }
        ExFreePoolWithTag(info, P5_TAG);
    }
    ZwClose(key);
}

// A DWORD from any key (absolute registry path).
static ULONG RegGetAbs(PCWSTR keyPath, PCWSTR name, ULONG fallback)
{
    UNICODE_STRING p, n;
    RtlInitUnicodeString(&p, keyPath);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &p, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE key;
    if (!NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa))) return fallback;
    RtlInitUnicodeString(&n, name);
    struct { KEY_VALUE_PARTIAL_INFORMATION info; UCHAR more[8]; } buf;
    ULONG got = 0, value = fallback;
    if (NT_SUCCESS(ZwQueryValueKey(key, &n, KeyValuePartialInformation, &buf, sizeof buf, &got)) && buf.info.Type == REG_DWORD && buf.info.DataLength == sizeof(ULONG))
        RtlCopyMemory(&value, buf.info.Data, sizeof value);
    ZwClose(key);
    return value;
}

// The volatile Session subkey (what wddm-mode.cmd writes): gone at the next Windows start.
static void SessionSet(PCWSTR name, ULONG value)
{
    WCHAR path[300];
    if (!NT_SUCCESS(RtlStringCbPrintfW(path, sizeof path, L"%wZ\\Session", &gRegPath))) return;
    UNICODE_STRING p, n;
    RtlInitUnicodeString(&p, path);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &p, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE key;
    ULONG disp = 0;
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_READ | KEY_WRITE, &oa, 0, NULL, REG_OPTION_VOLATILE, &disp))) return;
    RtlInitUnicodeString(&n, name);
    ZwSetValueKey(key, &n, 0, REG_DWORD, &value, sizeof value);
    ZwClose(key);
}

static NTSTATUS OpenFileAt(PCWSTR path, ACCESS_MASK access, ULONG share, ULONG options, HANDLE* h)
{
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, path);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    IO_STATUS_BLOCK io;
    return ZwCreateFile(h, access | SYNCHRONIZE, &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL, share, FILE_OPEN, options | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
}

// C:\ps5\gpu by way of \SystemRoot (e.g. \Device\BootDevice\Windows -> \Device\BootDevice\ps5\gpu): C: is the volume
// Windows runs from, and this early in the boot the C: drive letter is the one thing not certain to exist yet.
// Falls back to \??\C:\ps5\gpu.
static void BootPaths()
{
    RtlStringCbCopyW(gPs5Dir, sizeof gPs5Dir, L"\\??\\C:\\ps5\\gpu");
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, L"\\SystemRoot");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &n, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE h;
    if (!NT_SUCCESS(ZwOpenSymbolicLinkObject(&h, GENERIC_READ, &oa))) return;
    WCHAR buf[128];
    UNICODE_STRING t;
    t.Buffer = buf;
    t.Length = 0;
    t.MaximumLength = sizeof buf - sizeof(WCHAR);
    ULONG len = 0;
    NTSTATUS st = ZwQuerySymbolicLinkObject(h, &t, &len);
    ZwClose(h);
    if (!NT_SUCCESS(st)) return;
    USHORT chars = t.Length / sizeof(WCHAR);
    buf[chars] = 0;
    while (chars && buf[chars - 1] != L'\\') --chars;              // drop the last component ("Windows")
    if (chars < 2) return;
    buf[chars - 1] = 0;
    WCHAR dir[160];
    if (NT_SUCCESS(RtlStringCbPrintfW(dir, sizeof dir, L"%ws\\ps5\\gpu", buf))) RtlStringCbCopyW(gPs5Dir, sizeof gPs5Dir, dir);
}

// gPs5Dir + "\" + name
static PCWSTR BootPath(WCHAR* out, SIZE_T outBytes, PCWSTR name)
{
    if (!NT_SUCCESS(RtlStringCbPrintfW(out, outBytes, L"%ws\\%ws", gPs5Dir, name))) out[0] = 0;
    return out;
}

static BOOLEAN FileExists(PCWSTR path)
{
    HANDLE h;
    if (!NT_SUCCESS(OpenFileAt(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, &h))) return FALSE;
    ZwClose(h);
    return TRUE;
}

static NTSTATUS LatchOpen(HANDLE* h)
{
    WCHAR path[200];
    return OpenFileAt(BootPath(path, sizeof path, L"gpuboot.latch"), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                      FILE_NO_INTERMEDIATE_BUFFERING | FILE_WRITE_THROUGH | FILE_NON_DIRECTORY_FILE, h);
}

// The latch's first line, trailing blanks removed. Uncached I/O wants a sector-aligned buffer: a page from the pool.
static NTSTATUS LatchRead(HANDLE h, CHAR* line, ULONG lineBytes)
{
    line[0] = 0;
    UCHAR* blk = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, P5_TAG);
    if (!blk) return STATUS_INSUFFICIENT_RESOURCES;
    LARGE_INTEGER off;
    off.QuadPart = 0;
    IO_STATUS_BLOCK io;
    io.Information = 0;
    NTSTATUS st = ZwReadFile(h, NULL, NULL, NULL, &io, blk, PAGE_SIZE, &off, NULL);
    if (st == STATUS_END_OF_FILE) { st = STATUS_SUCCESS; io.Information = 0; }
    if (NT_SUCCESS(st)) {
        ULONG n = 0, got = (ULONG)(io.Information < PAGE_SIZE ? io.Information : PAGE_SIZE);
        while (n < got && n + 1 < lineBytes && blk[n] != '\r' && blk[n] != '\n' && blk[n] != 0) { line[n] = (CHAR)blk[n]; ++n; }
        while (n && line[n - 1] == ' ') --n;
        line[n] = 0;
    }
    ExFreePoolWithTag(blk, P5_TAG);
    return st;
}

// Rewrites the latch's 4096 bytes in place: one line of text, blanks, CR LF. Write-through, no cache.
static NTSTATUS LatchWrite(HANDLE h, const char* fmt, ...)
{
    UCHAR* blk = (UCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, P5_TAG);
    if (!blk) return STATUS_INSUFFICIENT_RESOURCES;
    RtlFillMemory(blk, PAGE_SIZE, ' ');
    CHAR text[300];
    va_list ap;
    va_start(ap, fmt);
    if (!NT_SUCCESS(RtlStringCbVPrintfA(text, sizeof text, fmt, ap))) text[sizeof text - 1] = 0;
    va_end(ap);
    SIZE_T n = 0;
    RtlStringCbLengthA(text, sizeof text, &n);
    RtlCopyMemory(blk, text, n);
    blk[PAGE_SIZE - 2] = '\r';
    blk[PAGE_SIZE - 1] = '\n';
    LARGE_INTEGER off;
    off.QuadPart = 0;
    IO_STATUS_BLOCK io;
    NTSTATUS st = ZwWriteFile(h, NULL, NULL, NULL, &io, blk, PAGE_SIZE, &off, NULL);
    ExFreePoolWithTag(blk, P5_TAG);
    return st;
}

static void BootLine(const char* text)
{
    SIZE_T n = 0;
    RtlStringCbLengthA(text, 240, &n);
    if (gBootLogLen + n + 2 < sizeof gBootLog) {
        RtlCopyMemory(gBootLog + gBootLogLen, text, n);
        gBootLogLen += (ULONG)n;
        gBootLog[gBootLogLen++] = '|';
        gBootLog[gBootLogLen] = 0;
    }
    LogLine(text, TRUE);
}

static void BootSay(const char* fmt, ...)
{
    CHAR line[240];
    va_list ap;
    va_start(ap, fmt);
    if (!NT_SUCCESS(RtlStringCbVPrintfA(line, sizeof line, fmt, ap))) line[sizeof line - 1] = 0;
    va_end(ap);
    BootLine(line);
}

// ---- what gpuboot.cpp needs, in kernel terms ----

static bool KbRd(void* ctx, uint32_t reg, uint32_t* v)
{
    BootHw* h = (BootHw*)ctx;
    // The rule of ps5gpu.sys and RegForbidden: nothing in the PSP/SMU block, no index/data pairs.
    if (!h->regs || reg >= h->regDwords || (reg >= 0x16000 && reg <= 0x17FFF) || reg <= 1 || (reg >= 0xC && reg <= 0xF)) { *v = 0xFFFFFFFF; return false; }
    *v = READ_REGISTER_ULONG((PULONG)&h->regs[reg]);
    return true;
}

static bool KbWr(void* ctx, uint32_t reg, uint32_t v)
{
    BootHw* h = (BootHw*)ctx;
    if (!h->regs || reg >= h->regDwords || (reg >= 0x16000 && reg <= 0x17FFF) || reg <= 1 || (reg >= 0xC && reg <= 0xF)) return false;
    WRITE_REGISTER_ULONG((PULONG)&h->regs[reg], v);
    return true;
}

static uint64_t KbNow(void*)
{
    LARGE_INTEGER f;
    LARGE_INTEGER t = KeQueryPerformanceCounter(&f);
    ULONGLONG ticks = (ULONGLONG)t.QuadPart, freq = (ULONGLONG)f.QuadPart;
    return (ticks / freq) * 1000000ull + (ticks % freq) * 1000000ull / freq;
}

// As stage2's backend: short waits spin, 5 ms and more sleep.
static void KbDelay(void* ctx, uint32_t us)
{
    if (us >= 5000) {
        LARGE_INTEGER w;
        w.QuadPart = -(LONGLONG)us * 10;
        KeDelayExecutionThread(KernelMode, FALSE, &w);
        return;
    }
    uint64_t end = KbNow(ctx) + us;
    while (KbNow(ctx) < end) KeStallExecutionProcessor(1);
}

// TRUE if [phys, phys + bytes) shares a page with memory Windows manages as RAM (ps5gpu.sys has the same check).
static BOOLEAN OverlapsRam(ULONGLONG phys, ULONGLONG bytes)
{
    BOOLEAN hit = FALSE;
    PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
    if (!ranges) return TRUE;
    for (ULONG i = 0; ranges[i].BaseAddress.QuadPart != 0 || ranges[i].NumberOfBytes.QuadPart != 0; ++i) {
        ULONGLONG b = (ULONGLONG)ranges[i].BaseAddress.QuadPart, n = (ULONGLONG)ranges[i].NumberOfBytes.QuadPart;
        if (phys < b + n && b < phys + bytes) hit = TRUE;
    }
    ExFreePool(ranges);
    return hit;
}

// Only the carve-out (above 4 GiB, below 64 GiB, never RAM), write-combined as every other mapping of it.
static uint8_t* KbMap(void*, uint64_t phys, uint64_t bytes)
{
    if (!bytes || (phys & 0xFFF) || phys < 0x100000000ull || phys + bytes > 0x1000000000ull || phys + bytes < phys || OverlapsRam(phys, bytes)) return nullptr;
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)phys;
    return (uint8_t*)MmMapIoSpace(pa, (SIZE_T)bytes, MmWriteCombined);
}

static void KbUnmap(void*, uint8_t* va, uint64_t bytes)
{
    if (va) MmUnmapIoSpace(va, (SIZE_T)bytes);
}

static bool KbDoorbell(void* ctx, uint32_t off, uint64_t value)
{
    BootHw* h = (BootHw*)ctx;
    if (!h->bar2 || off > 0x10000 - 8 || (off & 7)) return false;
    *(volatile ULONG64*)(h->bar2 + off) = value;          // one 64-bit store, as stage2's backend
    KeMemoryBarrier();
    return true;
}

static void KbFlush(void*) { __builtin_ia32_sfence(); }
static void KbLine(void*, const char* text) { BootLine(text); }

static void KbStep(void* ctx, uint32_t n, const char* name)
{
    BootHw* h = (BootHw*)ctx;
    NTSTATUS st = LatchWrite(h->latch, "TRYING step %u (%s), Windows up %u ms, GPU start by ps5wddm in DriverEntry", n, name, h->uptimeMs);
    RegSet(L"GpuBootStep", n, TRUE);       // (reaches the disk only once Windows writes the registry; the latch always does)
    if (!NT_SUCCESS(st)) BootSay("latch write for step %u failed 0x%08X", n, st);
}

static int KbFormat(char* out, size_t bytes, const char* fmt, va_list ap)
{
    NTSTATUS st = RtlStringCbVPrintfA(out, bytes, fmt, ap);
    return NT_SUCCESS(st) || st == STATUS_BUFFER_OVERFLOW ? 0 : -1;
}

// ---- the decision ----

static BOOLEAN BootFinish(ULONG result, const char* reason)
{
    BootSay("boot start: %s", reason);
    RegSet(L"GpuBootResult", result, FALSE);
    RegSetSz(L"GpuBootReason", reason);
    RegSetSz(L"GpuBootLog", gBootLog);
    LogFlush();
    return result == 1;
}

static BOOLEAN BootSkip(ULONG result, const char* fmt, ...)
{
    CHAR reason[220];
    va_list ap;
    va_start(ap, fmt);
    if (!NT_SUCCESS(RtlStringCbVPrintfA(reason, sizeof reason, fmt, ap))) reason[sizeof reason - 1] = 0;
    va_end(ap);
    return BootFinish(result, reason);
}

// PCI 20:00.0 by configuration reads only (no write, as ps5gpu.sys): id, memory decoding, BAR2 (doorbells), BAR5
// (registers). Before AddDevice Windows has handed this driver no resources yet, hence the direct reads.
static BOOLEAN BootFindGpu(ULONGLONG* bar2, ULONGLONG* bar5, CHAR* why, SIZE_T whyBytes)
{
    PCI_SLOT_NUMBER slot;
    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = 0;
    slot.u.bits.FunctionNumber = 0;
    ULONG cfg[10];
    RtlZeroMemory(cfg, sizeof cfg);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    ULONG got = HalGetBusDataByOffset(PCIConfiguration, 0x20, slot.u.AsULONG, cfg, 0, sizeof cfg);
#pragma clang diagnostic pop
    if (got != sizeof cfg) { RtlStringCbPrintfA(why, whyBytes, "PCI 20:00.0 configuration not readable (%u bytes)", got); return FALSE; }
    const ULONG* raw = cfg + 4;
    if (cfg[0] != 0x13FC1002) { RtlStringCbPrintfA(why, whyBytes, "not the PS5's GPU at PCI 20:00.0 (id %08X)", cfg[0]); return FALSE; }
    if (!(cfg[1] & 2)) { RtlStringCbPrintfA(why, whyBytes, "GPU memory decoding is off (command %08X)", cfg[1]); return FALSE; }
    *bar5 = raw[5] & ~0xFull;
    *bar2 = (raw[2] & 6) == 4 ? (((ULONGLONG)raw[3] << 32) | (raw[2] & ~0xFull)) : (raw[2] & ~0xFull);
    if ((raw[5] & 1) || !*bar5 || (raw[2] & 1) || !*bar2) { RtlStringCbPrintfA(why, whyBytes, "GPU BARs not usable (BAR2 %08X %08X, BAR5 %08X)", raw[2], raw[3], raw[5]); return FALSE; }
    return TRUE;
}

static NTSTATUS ReadFirmware(const WCHAR* dir, const char* name, UCHAR** data, SIZE_T* bytes)
{
    *data = NULL;
    *bytes = 0;
    WCHAR path[320];
    SIZE_T dl = 0;
    if (!NT_SUCCESS(RtlStringCbLengthW(dir, sizeof path, &dl))) return STATUS_NAME_TOO_LONG;
    dl /= sizeof(WCHAR);
    SIZE_T i = 0;
    for (; i < dl && i + 1 < ARRAYSIZE(path); ++i) path[i] = dir[i];
    if (i + 1 < ARRAYSIZE(path)) path[i++] = L'\\';
    for (const char* s = name; *s && i + 1 < ARRAYSIZE(path); ++s) path[i++] = (WCHAR)(UCHAR)*s;
    path[i] = 0;
    HANDLE h;
    NTSTATUS st = OpenFileAt(path, GENERIC_READ, FILE_SHARE_READ, FILE_NON_DIRECTORY_FILE, &h);
    if (!NT_SUCCESS(st)) return st;
    FILE_STANDARD_INFORMATION si;
    IO_STATUS_BLOCK io;
    st = ZwQueryInformationFile(h, &io, &si, sizeof si, FileStandardInformation);
    if (NT_SUCCESS(st) && (si.EndOfFile.QuadPart <= 0 || si.EndOfFile.QuadPart > (LONGLONG)kFwMaxBytes)) st = STATUS_INVALID_IMAGE_FORMAT;
    if (NT_SUCCESS(st)) {
        SIZE_T n = (SIZE_T)si.EndOfFile.QuadPart;
        UCHAR* buf = (UCHAR*)ExAllocatePool2(POOL_FLAG_PAGED, n, P5_TAG);
        if (!buf) st = STATUS_INSUFFICIENT_RESOURCES;
        else {
            LARGE_INTEGER off;
            off.QuadPart = 0;
            io.Information = 0;
            st = ZwReadFile(h, NULL, NULL, NULL, &io, buf, (ULONG)n, &off, NULL);
            if (NT_SUCCESS(st) && io.Information != n) st = STATUS_END_OF_FILE;
            if (NT_SUCCESS(st)) { *data = buf; *bytes = n; }
            else ExFreePoolWithTag(buf, P5_TAG);
        }
    }
    ZwClose(h);
    return st;
}

static BOOLEAN BootStartGpu()
{
    SessionSet(L"BootTried", 1);
    gBootLogLen = 0;
    gBootLog[0] = 0;
    BootPaths();
    BootHw hw;
    RtlZeroMemory(&hw, sizeof hw);
    hw.uptimeMs = (ULONG)(KeQueryInterruptTime() / 10000);
    BOOLEAN live = RegGetSession(L"BootTest", 0) == 1;
    RegSet(L"GpuBootUptimeMs", hw.uptimeMs, FALSE);
    RegSet(L"GpuBootTrusted", 0, FALSE);
    RegSet(L"GpuBootStep", 0, FALSE);
    RegSet(L"GpuBootMs", 0, FALSE);
    BootSay("boot start: Windows up %u ms%s", hw.uptimeMs, live ? " (live test: Session\\BootTest=1)" : "");

    if (RegGet(L"GpuBootStart", 1) == 0) return BootSkip(2, "switched off (service value GpuBootStart=0)");
    if (RegGet(L"GpuBootOff", 0) == 1) return BootSkip(4, "latched off after a start that did not finish (GpuBootOff=1; gpu-bootstart.cmd on re-arms)");
    if (!live && hw.uptimeMs > kBootWindowMs) return BootSkip(2, "not at boot (Windows up %u s); a live test needs Session\\BootTest=1", hw.uptimeMs / 1000);
    if (RegGet(L"StartPending", 0) != 0) return BootSkip(2, "the previous start of this driver did not finish (StartPending=1)");
    CHAR why[160];
    ULONGLONG bar2 = 0, bar5 = 0;
    if (!BootFindGpu(&bar2, &bar5, why, sizeof why)) return BootSkip(2, "%s", why);
    WCHAR path[200];
    BootSay("files: %ws", gPs5Dir);
    if (!FileExists(BootPath(path, sizeof path, L"umd-dwm-on"))) return BootSkip(2, "C:\\ps5\\gpu\\umd-dwm-on not found (the compositor may not use the Direct3D driver)");
    if (FileExists(BootPath(path, sizeof path, L"autostart.nofull"))) return BootSkip(2, "C:\\ps5\\gpu\\autostart.nofull: display-only wanted");
    if (FileExists(BootPath(path, sizeof path, L"autostart.off"))) return BootSkip(2, "C:\\ps5\\gpu\\autostart.off: the user-mode start hung once; not trying here either");
    ULONG shimStart = RegGetAbs(L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\ps5gpu", L"Start", 0xFFFFFFFF);
    if (shimStart != 1) return BootSkip(2, "kernel shim ps5gpu not set to load at boot (Start=%d; gpu-bootstart.cmd arm sets 1)", (int)shimStart);

    NTSTATUS st = LatchOpen(&hw.latch);
    if (st == STATUS_OBJECT_NAME_NOT_FOUND || st == STATUS_OBJECT_PATH_NOT_FOUND) return BootSkip(2, "not armed: C:\\ps5\\gpu\\gpuboot.latch not found (0x%08X)", st);
    if (!NT_SUCCESS(st)) return BootSkip(2, "latch file cannot be opened (0x%08X)", st);
    CHAR latch[160];
    st = LatchRead(hw.latch, latch, sizeof latch);
    if (!NT_SUCCESS(st)) { ZwClose(hw.latch); return BootSkip(2, "latch file cannot be read (0x%08X)", st); }
    BootSay("latch: %s", latch);
    if (!strncmp(latch, "TRYING", 6)) {
        // The start at the previous boot (or its first 20 s of full mode) never finished: crash, hang or power cut.
        st = LatchWrite(hw.latch, "OFF the start at the previous boot did not finish (it said: %s); gpu-bootstart.cmd on re-arms", latch);
        ZwClose(hw.latch);
        RegSet(L"GpuBootOff", 1, TRUE);
        return BootSkip(4, "the start at the previous boot did not finish (%s): latched off (latch write 0x%08X)", latch, st);
    }
    if (!strncmp(latch, "OFF", 3)) { ZwClose(hw.latch); return BootSkip(4, "latched off: %s", latch); }
    if (strncmp(latch, "IDLE", 4)) { ZwClose(hw.latch); return BootSkip(2, "latch file says neither IDLE, TRYING nor OFF"); }

    WCHAR dir[200];
    RegGetSz(L"GpuBootFwDir", dir, sizeof dir);
    if (!dir[0]) BootPath(dir, sizeof dir, L"fw");
    gpuboot::Files files;
    RtlZeroMemory(&files, sizeof files);
    UCHAR* fwBuf[gpuboot::kFwCount] = {};
    BOOLEAN fwOk = TRUE;
    for (int i = 0; i < gpuboot::kFwCount && fwOk; ++i) {
        SIZE_T n = 0;
        st = ReadFirmware(dir, gpuboot::kFwNames[i], &fwBuf[i], &n);
        if (!NT_SUCCESS(st)) { fwOk = FALSE; RtlStringCbPrintfA(why, sizeof why, "microcode %s not readable (0x%08X)", gpuboot::kFwNames[i], st); break; }
        files.data[i] = fwBuf[i];
        files.bytes[i] = n;
    }
    PHYSICAL_ADDRESS pa;
    if (fwOk) {
        pa.QuadPart = (LONGLONG)bar5;
        hw.regs = (volatile ULONG*)MmMapIoSpace(pa, 0x80000, MmNonCached);
        hw.regDwords = 0x80000 / 4;
        pa.QuadPart = (LONGLONG)bar2;
        hw.bar2 = (volatile UCHAR*)MmMapIoSpace(pa, 0x10000, MmNonCached);
        if (!hw.regs || !hw.bar2) { fwOk = FALSE; RtlStringCbPrintfA(why, sizeof why, "GPU registers or doorbells cannot be mapped"); }
    }
    if (fwOk) {
        st = LatchWrite(hw.latch, "TRYING step 0 (begin), Windows up %u ms, GPU start by ps5wddm in DriverEntry", hw.uptimeMs);
        if (!NT_SUCCESS(st)) { fwOk = FALSE; RtlStringCbPrintfA(why, sizeof why, "latch file not writable (0x%08X): not starting without it", st); }
    }
    gpuboot::Result r = gpuboot::kBadFirmware;
    CHAR reason[200];
    reason[0] = 0;
    ULONGLONG t0 = KeQueryInterruptTime();
    if (fwOk) {
        gpuboot::Env env = { &hw, KbRd, KbWr, KbDelay, KbNow, KbMap, KbUnmap, KbDoorbell, KbFlush, KbLine, KbStep, KbFormat };
        gpuboot::Options opt = { false, live ? true : false, kBootBudgetMs };
        r = gpuboot::run(env, files, opt, reason, sizeof reason);
    }
    ULONG ms = (ULONG)((KeQueryInterruptTime() - t0) / 10000);
    RegSet(L"GpuBootMs", ms, FALSE);
    if (hw.regs) MmUnmapIoSpace((PVOID)hw.regs, 0x80000);
    if (hw.bar2) MmUnmapIoSpace((PVOID)hw.bar2, 0x10000);
    for (int i = 0; i < gpuboot::kFwCount; ++i)
        if (fwBuf[i]) ExFreePoolWithTag(fwBuf[i], P5_TAG);
    if (!fwOk) { ZwClose(hw.latch); return BootSkip(2, "%s", why); }
    if (r != gpuboot::kOk) {
        // A start that fails cleanly is not a hang: armed again for the next boot (as autostart.cmd retries a failure).
        st = LatchWrite(hw.latch, "IDLE last boot start failed: %s", reason);
        ZwClose(hw.latch);
        return BootSkip(3, "GPU start failed (%u): %s; display-only, autostart.cmd starts it the old way", (unsigned)r, reason);
    }
    st = LatchWrite(hw.latch, "TRYING step 9 (full mode until trusted), Windows up %u ms, GPU started in %u ms", hw.uptimeMs, ms);
    ZwClose(hw.latch);
    gBootStarted = TRUE;
    gBootSettled = 0;
    SessionSet(L"FullDriver", 1);            // later reloads in this session stay full (wddm-mode.cmd can still switch)
    SessionSet(L"BootStart", 1);             // for autostart.cmd: the GPU is up, no ps5gpu2 run, no device restart
    return BootSkip(1, "%s; full mode from the first start (latch 0x%08X)", reason, st);
}

// After a boot start: full mode has run 20 s (trusted), or the device stopped or failed before that. Either way the
// start did not take Windows down: the latch goes back to IDLE (armed for the next boot).
static BOOLEAN ShimPresent()
{
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\Device\\Ps5Gpu");
    PFILE_OBJECT fo = NULL;
    PDEVICE_OBJECT dev = NULL;
    if (!NT_SUCCESS(IoGetDeviceObjectPointer(&name, FILE_READ_ATTRIBUTES, &fo, &dev))) return FALSE;
    ObDereferenceObject(fo);
    return TRUE;
}

static void BootSettle(BOOLEAN trusted, const char* why)
{
    if (!gBootStarted || KeGetCurrentIrql() != PASSIVE_LEVEL || InterlockedExchange(&gBootSettled, 1)) return;
    BOOLEAN shim = ShimPresent();
    HANDLE h;
    NTSTATUS st = LatchOpen(&h);
    if (NT_SUCCESS(st)) {
        st = LatchWrite(h, "IDLE after the last boot start: GPU started at boot, then %s; kernel shim %s", why, shim ? "loaded" : "NOT loaded");
        ZwClose(h);
    }
    RegSet(L"GpuBootShim", shim, FALSE);
    if (trusted) RegSet(L"GpuBootTrusted", 1, TRUE);
    LogF("boot start: %s; kernel shim %s; latch back to IDLE (0x%08X)", why, shim ? "loaded" : "NOT loaded", st);
    LogFlush();
}

#ifdef P5_OWN_COOKIE
// Stack-overrun check without Microsoft's bufferoverflowfastfailk.lib. That library brings a "load configuration"
// block along, and the image then fails to load on Windows 10 19041 when linked with lld (STATUS_CONFLICTING_ADDRESSES,
// found in the VM rehearsal). Microsoft's own code in displib/ntoskrnl.lib is compiled with the check and needs these.
// The check must keep every register but rcx (the caller's return value is in rax), hence assembly.
extern "C" {
ULONG_PTR __security_cookie = 0x00002B992DDFA232ull;
ULONG_PTR __security_cookie_complement = ~0x00002B992DDFA232ull;
DECLSPEC_NORETURN void P5CookieFail(ULONG_PTR found) { KeBugCheckEx(0xF7 /* DRIVER_OVERRAN_STACK_BUFFER */, found, __security_cookie, 0, 0); }
}
__asm__(".text\n.globl __security_check_cookie\n__security_check_cookie:\n.intel_syntax noprefix\n  cmp rcx, qword ptr [rip + __security_cookie]\n  jne P5CookieFail\n  ret\n.att_syntax\n");
#endif

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath)
{
    gRegPath.Buffer = gRegPathBuf;
    gRegPath.MaximumLength = sizeof gRegPathBuf;
    RtlCopyUnicodeString(&gRegPath, registryPath);
    RegSet(L"Entry", 1, TRUE);                  // DriverEntry ran: the image loaded

    KeInitializeSpinLock(&gLogLock);
    RegSet(L"GpuBootCapable", 1, FALSE);       // for autostart.cmd: this driver can start the GPU at boot
    ULONG fullSwitch = RegGetSession(L"FullDriver", 0xFFFFFFFF);
    gFull = fullSwitch == 1;
    // No explicit switch yet in this Windows session: try the GPU start here (once per session); full mode if it worked.
    if (fullSwitch == 0xFFFFFFFF && RegGetSession(L"BootTried", 0) == 0) gFull = BootStartGpu();
    if (gFull) {
        DRIVER_INITIALIZATION_DATA f;
        RtlZeroMemory(&f, sizeof f);
        // WDDM 1.3, as viogpu3d registers (it runs on Windows 10 and 11 that way). The first 1.3 tries on 2026-11-20 were
        // stopped at once, but that was the missing UserModeDriverName in the INF. Registered as 2.0 (FullVersion=20) the
        // console went down right after the segment questions: 2.0 expects GPU virtual addressing, which this is not.
        gWddm2 = RegGet(L"FullVersion", 13) == 20;
        f.Version = gWddm2 ? DXGKDDI_INTERFACE_VERSION_WDDM2_0 : DXGKDDI_INTERFACE_VERSION_WDDM1_3;
        f.DxgkDdiAddDevice = DdiAddDevice;
        f.DxgkDdiStartDevice = DdiStartDevice;
        f.DxgkDdiStopDevice = DdiStopDevice;
        f.DxgkDdiRemoveDevice = DdiRemoveDevice;
        f.DxgkDdiDispatchIoRequest = DdiDispatchIoRequest;
        f.DxgkDdiInterruptRoutine = DdiInterruptRoutine;
        f.DxgkDdiDpcRoutine = FullDpcRoutine;
        f.DxgkDdiQueryChildRelations = DdiQueryChildRelations;
        f.DxgkDdiQueryChildStatus = DdiQueryChildStatus;
        f.DxgkDdiQueryDeviceDescriptor = DdiQueryDeviceDescriptor;
        f.DxgkDdiSetPowerState = DdiSetPowerState;
        f.DxgkDdiNotifyAcpiEvent = DdiNotifyAcpiEvent;
        f.DxgkDdiResetDevice = DdiResetDevice;
        f.DxgkDdiUnload = DdiUnload;
        f.DxgkDdiQueryInterface = DdiQueryInterface;
        f.DxgkDdiQueryAdapterInfo = FullQueryAdapterInfo;
        f.DxgkDdiEscape = DdiEscape;
        f.DxgkDdiCreateDevice = DdiCreateDevice;
        f.DxgkDdiDestroyDevice = DdiDestroyDevice;
        f.DxgkDdiCreateContext = DdiCreateContext;
        f.DxgkDdiDestroyContext = DdiDestroyContext;
        f.DxgkDdiCreateAllocation = DdiCreateAllocation;
        f.DxgkDdiOpenAllocation = DdiOpenAllocation;
        f.DxgkDdiCloseAllocation = DdiCloseAllocation;
        f.DxgkDdiDescribeAllocation = DdiDescribeAllocation;
        f.DxgkDdiDestroyAllocation = DdiDestroyAllocation;
        f.DxgkDdiGetStandardAllocationDriverData = DdiGetStandardAllocationDriverData;
        f.DxgkDdiBuildPagingBuffer = DdiBuildPagingBuffer;
        f.DxgkDdiPresent = DdiPresent;
        f.DxgkDdiRender = DdiRender;
        f.DxgkDdiRenderKm = DdiRenderKm;
        f.DxgkDdiPatch = DdiPatch;
        f.DxgkDdiSubmitCommand = DdiSubmitCommand;
        f.DxgkDdiPreemptCommand = DdiPreemptCommand;
        f.DxgkDdiQueryCurrentFence = DdiQueryCurrentFence;
        f.DxgkDdiResetFromTimeout = DdiResetFromTimeout;
        f.DxgkDdiRestartFromTimeout = DdiRestartFromTimeout;
        f.DxgkDdiCollectDbgInfo = DdiCollectDbgInfo;
        f.DxgkDdiQueryEngineStatus = DdiQueryEngineStatus;
        f.DxgkDdiResetEngine = DdiResetEngine;
        f.DxgkDdiCancelCommand = DdiCancelCommand;
        f.DxgkDdiGetNodeMetadata = DdiGetNodeMetadata;
        f.DxgkDdiControlInterrupt = DdiControlInterrupt;
        f.DxgkDdiGetScanLine = DdiGetScanLine;
        f.DxgkDdiRecommendVidPnTopology = DdiRecommendVidPnTopology;
        f.DxgkDdiStopCapture = DdiStopCapture;
        f.DxgkDdiSetPalette = DdiSetPalette;
        f.DxgkDdiAcquireSwizzlingRange = DdiAcquireSwizzlingRange;
        f.DxgkDdiReleaseSwizzlingRange = DdiReleaseSwizzlingRange;
        f.DxgkDdiQueryDependentEngineGroup = DdiQueryDependentEngineGroup;
        f.DxgkDdiSetPointerPosition = DdiSetPointerPosition;
        f.DxgkDdiSetPointerShape = DdiSetPointerShape;
        f.DxgkDdiIsSupportedVidPn = DdiIsSupportedVidPn;
        f.DxgkDdiRecommendFunctionalVidPn = DdiRecommendFunctionalVidPn;
        f.DxgkDdiEnumVidPnCofuncModality = DdiEnumVidPnCofuncModality;
        f.DxgkDdiSetVidPnSourceAddress = DdiSetVidPnSourceAddress;
        f.DxgkDdiSetVidPnSourceVisibility = FullSetVidPnSourceVisibility;
        f.DxgkDdiCommitVidPn = DdiCommitVidPn;
        f.DxgkDdiUpdateActiveVidPnPresentPath = DdiUpdateActiveVidPnPresentPath;
        f.DxgkDdiRecommendMonitorModes = DdiRecommendMonitorModes;
        f.DxgkDdiQueryVidPnHWCapability = DdiQueryVidPnHWCapability;
        f.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = DdiStopDeviceAndReleasePostDisplayOwnership;
        f.DxgkDdiSystemDisplayEnable = DdiSystemDisplayEnable;
        f.DxgkDdiSystemDisplayWrite = DdiSystemDisplayWrite;
        NTSTATUS fst = DxgkInitialize(driver, registryPath, &f);
        RegSet(L"EntryStatus", (ULONG)fst, TRUE);
        RegSet(L"EntryFull", 1, TRUE);
        return fst;
    }
    RegSet(L"EntryFull", 0, FALSE);
    KMDDOD_INITIALIZATION_DATA init;
    RtlZeroMemory(&init, sizeof init);
    init.Version = DXGKDDI_INTERFACE_VERSION_WIN8;
    init.DxgkDdiAddDevice = DdiAddDevice;
    init.DxgkDdiStartDevice = DdiStartDevice;
    init.DxgkDdiStopDevice = DdiStopDevice;
    init.DxgkDdiRemoveDevice = DdiRemoveDevice;
    init.DxgkDdiDispatchIoRequest = DdiDispatchIoRequest;
    init.DxgkDdiInterruptRoutine = DdiInterruptRoutine;
    init.DxgkDdiDpcRoutine = DdiDpcRoutine;
    init.DxgkDdiQueryChildRelations = DdiQueryChildRelations;
    init.DxgkDdiQueryChildStatus = DdiQueryChildStatus;
    init.DxgkDdiQueryDeviceDescriptor = DdiQueryDeviceDescriptor;
    init.DxgkDdiSetPowerState = DdiSetPowerState;
    init.DxgkDdiNotifyAcpiEvent = DdiNotifyAcpiEvent;
    init.DxgkDdiResetDevice = DdiResetDevice;
    init.DxgkDdiUnload = DdiUnload;
    init.DxgkDdiQueryInterface = DdiQueryInterface;
    init.DxgkDdiQueryAdapterInfo = DdiQueryAdapterInfo;
    init.DxgkDdiSetPointerPosition = DdiSetPointerPosition;
    init.DxgkDdiSetPointerShape = DdiSetPointerShape;
    init.DxgkDdiIsSupportedVidPn = DdiIsSupportedVidPn;
    init.DxgkDdiRecommendFunctionalVidPn = DdiRecommendFunctionalVidPn;
    init.DxgkDdiEnumVidPnCofuncModality = DdiEnumVidPnCofuncModality;
    init.DxgkDdiSetVidPnSourceVisibility = DdiSetVidPnSourceVisibility;
    init.DxgkDdiCommitVidPn = DdiCommitVidPn;
    init.DxgkDdiUpdateActiveVidPnPresentPath = DdiUpdateActiveVidPnPresentPath;
    init.DxgkDdiRecommendMonitorModes = DdiRecommendMonitorModes;
    init.DxgkDdiQueryVidPnHWCapability = DdiQueryVidPnHWCapability;
    init.DxgkDdiPresentDisplayOnly = DdiPresentDisplayOnly;
    init.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = DdiStopDeviceAndReleasePostDisplayOwnership;
    init.DxgkDdiSystemDisplayEnable = DdiSystemDisplayEnable;
    init.DxgkDdiSystemDisplayWrite = DdiSystemDisplayWrite;
    NTSTATUS st = DxgkInitializeDisplayOnlyDriver(driver, registryPath, &init);
    RegSet(L"EntryStatus", (ULONG)st, TRUE);
    return st;
}
