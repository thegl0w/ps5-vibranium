// SPDX-License-Identifier: MIT
// Warm restart on the PS5 from Windows: "Restart comes back into Windows without leaving our firmware or the loader".
// Talks to the firmware's runtime variable Ps5WarmRestart (firmware/ps5warm): Set 1 byte 0/1; Get 16 bytes
// {valid, armed, loops, cap}, or 288 bytes with stage 1's PCI restore report (firmware/ps5warm/warm_pci.h, WPD_*).
//   ps5warm.exe          state, and on a warm run what Windows' shutdown had left in the USB controllers
//   ps5warm.exe on       Restart loops back into Windows (until the next power-off; every start from Linux is unarmed)
//   ps5warm.exe off      Restart restarts the console; start the loader again, as in 1.0
// Exit code: 0 ok (for "on"/"off": the firmware took it), 1 failed, 2 no privilege / usage, 3 this firmware has no
// warm restart. Needs an administrator or SYSTEM (SeSystemEnvironmentPrivilege). KERNEL32 + ADVAPI32 only, no C runtime.
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

static char out[4096];
static U32 outn;
static void putc_(char c) { if (outn < sizeof out) out[outn++] = c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void putu(uint64_t v) { char t[21]; int i = 0; do { t[i++] = '0' + v % 10; v /= 10; } while (v); while (i) putc_(t[--i]); }
static void puthex(U32 v, int digits) { for (int i = (digits - 1) * 4; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static void flush(void) { U32 w; WriteFile(GetStdHandle((U32)-11), out, outn, &w, NULL); outn = 0; }
static void bdf(U32 b) { puthex((b >> 8) & 0xFF, 2); putc_(':'); puthex((b >> 3) & 31, 2); putc_('.'); puthex(b & 7, 1); }
static const wchar_t guid[] = L"{8c0f1e5a-5053-3543-8a6b-5f4e2d1c0b9a}";
static U32 buf[72];                                  // 16 bytes of state + 0x110 bytes of report
#define REP(o) (buf[4 + (o) / 4])

void entry(void) {
    Handle tok = NULL;
    int64_t luid = 0;
    U32 tp[4] = { 1, 0, 0, 2 };                      // TOKEN_PRIVILEGES: count, LUID (low, high), SE_PRIVILEGE_ENABLED
    int have = OpenProcessToken(GetCurrentProcess(), 0x28, &tok) && LookupPrivilegeValueW(NULL, L"SeSystemEnvironmentPrivilege", &luid);
    tp[1] = (U32)luid; tp[2] = (U32)(luid >> 32);
    if (!have || !AdjustTokenPrivileges(tok, 0, tp, 0, NULL, NULL) || GetLastError() != 0) {
        puts_("could not enable SeSystemEnvironmentPrivilege (run as administrator), error "); putu(GetLastError()); puts_("\r\n"); flush();
        ExitProcess(2);
    }
    int set = -1;
    const wchar_t *c = GetCommandLineW();
    if (c && *c == L'"') { for (++c; *c && *c != L'"'; ++c) {} if (*c) ++c; } else { while (c && *c && *c != L' ') ++c; }
    while (c && *c == L' ') ++c;
    if (c && *c) {
        if (c[0] == L'o' && c[1] == L'n' && (c[2] == 0 || c[2] == L' ')) set = 1;
        else if (c[0] == L'o' && c[1] == L'f' && c[2] == L'f' && (c[3] == 0 || c[3] == L' ')) set = 0;
        else if (!(c[0] == L's' && c[1] == L't')) { puts_("usage: ps5warm [status|on|off]\r\n"); flush(); ExitProcess(2); }
    }
    if (set >= 0) {
        uint8_t v = (uint8_t)set;
        if (!SetFirmwareEnvironmentVariableExW(L"Ps5WarmRestart", guid, &v, 1, 7)) {
            U32 e = GetLastError();
            puts_(set ? "warm restart on: FAILED, error " : "warm restart off: FAILED, error "); putu(e);
            puts_(e == 50 ? " (this firmware has no warm restart, or the launch left no valid record)\r\n" : "\r\n"); flush();
            ExitProcess(e == 50 ? 3 : 1);
        }
    }
    U32 attr = 0, n = GetFirmwareEnvironmentVariableExW(L"Ps5WarmRestart", guid, buf, sizeof buf, &attr);
    if (n != 16 && n != sizeof buf) {
        puts_("warm restart: the firmware did not answer (bytes "); putu(n); puts_(", error "); putu(GetLastError()); puts_(")\r\n"); flush();
        ExitProcess(3);
    }
    puts_("warm restart: valid "); putu(buf[0]); puts_("  armed "); putu(buf[1]); if (buf[1] > 1) { puts_(" (inherited; "); putu(buf[1] - 1); puts_(" start(s) in a row not confirmed, 'on' confirms)"); } puts_("  loops "); putu(buf[2]); puts_("  cap "); putu(buf[3]); puts_("\r\n");
    if (n == sizeof buf && REP(0) == 0x31445057u) {
        puts_("pci restore, stage run "); putu(REP(4)); puts_(": bus 20 as found "); puthex(REP(12), 8);
        puts_(" (per function: 8 present, 4 memory decoding on, low 2 bits = D-state)\r\n");
        for (int i = 0; i < 2; i++) {
            int o = 0x10 + i * 0x20;
            if (!REP(o)) continue;
            puts_("  root port "); bdf(REP(o)); puts_(": found cmd "); puthex(REP(o + 4), 4); puts_(" buses "); puthex(REP(o + 8), 6);
            puts_(" window "); puthex(REP(o + 12), 8); puts_(" pm "); puthex(REP(o + 16), 4); puts_("; flags "); puthex(REP(o + 20), 3);
            puts_((REP(o + 20) & 8) ? " (restored)\r\n" : " (as saved)\r\n");
        }
        for (U32 i = 0; i < REP(8) && i < 4; i++) {
            int o = 0x50 + (int)i * 0x30; U32 f = REP(o + 16);
            puts_("  xhci "); bdf(REP(o)); puts_(": found cmd "); puthex(REP(o + 4), 4); puts_(" pm "); puthex(REP(o + 8), 4);
            puts_(" bar "); puthex(REP(o + 12), 8); puts_("; flags "); puthex(f, 3); puts_(":");
            puts_((f & 2) ? " was asleep," : " was in D0,");
            if (!(f & 4)) puts_(" NOT D0 NOW,");
            if (f & 8) puts_(" header restored,");
            if (f & 0x10) puts_(" vendor bytes restored,");
            if (f & 0x400) puts_(" power-cycled for a second try,");
            if (!(f & 0x20)) puts_(" REGISTERS DEAD,");
            puts_((f & 0x80) ? " reset ok (" : (f & 0x800) ? " halted, not reset (" : " RESET FAILED ("); putu(REP(o + 24)); puts_(" us)");
            if (f & 0x100) puts_(", port power switched on");
            if (f & 0x1000) { puts_(", disk re-plugged by port power ("); puts_((f & 0x2000) ? "gone while off, " : "STAYED CONNECTED while off, "); if (f & 0x4000) { puts_("back after "); putu(REP(o + 24) / 1000); puts_(" ms)"); } else puts_("NOT BACK)"); }
            puts_("; usbsts "); puthex(REP(o + 20), 8); puts_("; "); putu(REP(o + 28)); puts_(" ports:");
            for (int p = 0; p < 4; p++) { putc_(' '); puthex(REP(o + 32 + p * 4), 8); }
            puts_("\r\n");
        }
    } else if (n == sizeof buf) {
        puts_("pci restore: no report (this Windows was started from Linux, not by a warm restart)\r\n");
    }
    flush();
    ExitProcess(buf[0] ? 0 : 3);
}
