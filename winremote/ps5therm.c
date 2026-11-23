// PS5 temperatures and fan state from Windows, through the firmware (firmware/ps5reset/ps5_varhook.inc): the UEFI variable
// "Ps5Thermal" is answered by our runtime code with a fresh reading from the console's controller. No driver needed.
//   ps5therm.exe            one reading
//   ps5therm.exe set auto   hand the fan to the controller (normal)
//   ps5therm.exe set max    full speed
// Needs an administrator (SeSystemEnvironmentPrivilege). KERNEL32 + ADVAPI32 only, no C runtime.
#include <stddef.h>
#include <stdint.h>
typedef uint32_t U32;
typedef void *Handle;
#define API __declspec(dllimport)
API U32 GetFirmwareEnvironmentVariableExW(const wchar_t *, const wchar_t *, void *, U32, U32 *);
API int SetFirmwareEnvironmentVariableExW(const wchar_t *, const wchar_t *, void *, U32, U32);
API Handle GetCurrentProcess(void);
API Handle GetStdHandle(U32);
API int WriteFile(Handle, const void *, U32, U32 *, void *);
API void ExitProcess(U32);
API const wchar_t *GetCommandLineW(void);
API U32 GetLastError(void);
API int OpenProcessToken(Handle, U32, Handle *);
API int LookupPrivilegeValueW(const wchar_t *, const wchar_t *, int64_t *);
API int AdjustTokenPrivileges(Handle, int, void *, U32, void *, U32 *);

static char out[2048];
static U32 outn;
static void putc_(char c) { if (outn < sizeof out) out[outn++] = c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void putu(uint64_t v) { char t[21]; int i = 0; do { t[i++] = '0' + v % 10; v /= 10; } while (v); while (i) putc_(t[--i]); }
static void puti(int64_t v) { if (v < 0) { putc_('-'); v = -v; } putu((uint64_t)v); }
static void puthex32(U32 v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static void flush(void) { U32 w; WriteFile(GetStdHandle((U32)-11), out, outn, &w, NULL); outn = 0; }
static void temp(const char *name, int32_t mc) {
    puts_(name); putc_(' ');
    if (mc < 0) { puts_("error "); puti(mc); } else { putu(mc / 1000); putc_('.'); putu((mc % 1000) / 100); puts_(" C"); }
}
#pragma pack(push, 1)
struct thermal { U32 version, flags; int32_t temp[5]; int32_t duty, mode; U32 bridge[4]; U32 calls, setcalls, reserved; };
#pragma pack(pop)
static const wchar_t guid[] = L"{8c0f1e5a-5053-3543-8a6b-5f4e2d1c0b9a}";

void entry(void) {
    Handle tok = NULL;
    int64_t luid = 0;
    U32 tp[4] = { 1, 0, 0, 2 };                 // TOKEN_PRIVILEGES: count, LUID (low, high), SE_PRIVILEGE_ENABLED
    int have = OpenProcessToken(GetCurrentProcess(), 0x28, &tok) && LookupPrivilegeValueW(NULL, L"SeSystemEnvironmentPrivilege", &luid);
    tp[1] = (U32)luid; tp[2] = (U32)(luid >> 32);
    if (!have || !AdjustTokenPrivileges(tok, 0, tp, 0, NULL, NULL) || GetLastError() != 0) {
        puts_("could not enable SeSystemEnvironmentPrivilege (run as administrator), error "); putu(GetLastError()); puts_("\r\n"); flush();
        ExitProcess(2);
    }
    int set = 0;
    for (const wchar_t *c = GetCommandLineW(); c && *c; ++c) {
        if (c[0] == L' ' && c[1] == L's' && c[2] == L'e' && c[3] == L't' && c[4] == L' ') {
            if (c[5] == L'a') set = 1;            // auto
            if (c[5] == L'm') set = 2;            // max
            if (!set) { puts_("usage: ps5therm [set auto|set max]\r\n"); flush(); ExitProcess(2); }
        }
    }
    if (set) {
        uint8_t mode = (uint8_t)set;
        int ok = SetFirmwareEnvironmentVariableExW(L"Ps5FanMode", guid, &mode, 1, 7);
        puts_(set == 1 ? "fan mode -> auto: " : "fan mode -> maximum: ");
        if (ok) puts_("ok\r\n"); else { puts_("FAILED, error "); putu(GetLastError()); puts_("\r\n"); }
        flush();
        if (!ok) ExitProcess(1);
    }
    struct thermal t;
    U32 attr = 0, n = GetFirmwareEnvironmentVariableExW(L"Ps5Thermal", guid, &t, sizeof t, &attr);
    if (n != sizeof t) {
        puts_("no reading: the firmware did not answer Ps5Thermal (bytes "); putu(n); puts_(", error "); putu(GetLastError()); puts_(")\r\n"); flush();
        ExitProcess(1);
    }
    temp("MainSoC", t.temp[0]); puts_(" | "); temp("Local1", t.temp[1]); puts_(" | "); temp("Local2", t.temp[2]); puts_(" | ");
    temp("Local3", t.temp[3]); puts_(" | "); temp("S18", t.temp[4]); puts_("\r\n");
    puts_("fan duty ");
    if (t.duty < 0) { puts_("error "); puti(t.duty); } else { putu(t.duty); puts_("/1024 ("); putu((t.duty * 100 + 512) / 1024); puts_("%)"); }
    puts_(" | mode ");
    puts_(t.mode == 1 ? "auto" : t.mode == 2 ? "maximum" : t.mode == 4 ? "manual" : t.mode < 0 ? "error" : "other"); puts_(" ("); puti(t.mode); puts_(")");
    puts_(" | flags 0x"); puthex32(t.flags); puts_((t.flags & 0x80000000u) ? " MOCK" : "");
    puts_(" | reads "); putu(t.calls); puts_(" sets "); putu(t.setcalls); puts_("\r\n");
    puts_("bridge cmd "); puthex32(t.bridge[0]); puts_(" buses "); puthex32(t.bridge[1]); puts_(" mem "); puthex32(t.bridge[2]); puts_(" pref "); puthex32(t.bridge[3]); puts_("\r\n");
    flush();
    ExitProcess(0);
}
