/* SPDX-License-Identifier: MIT */
/* ntload <service name>: asks the kernel to load a driver service and prints the exact NTSTATUS (the service manager
 * only gives a lossy Win32 error). ntload -u <service name> unloads. For diagnosing drivers that will not start. */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } USTR;
NTSYSAPI LONG NTAPI NtLoadDriver(USTR *name);
NTSYSAPI LONG NTAPI NtUnloadDriver(USTR *name);
NTSYSAPI LONG NTAPI RtlAdjustPrivilege(ULONG privilege, BOOLEAN enable, BOOLEAN thread, PBOOLEAN was);

int main(int argc, char **argv)
{
    int unload = argc > 2 && !strcmp(argv[1], "-u");
    if (argc < 2 + unload) { printf("usage: ntload [-u] <service>\n"); return 2; }
    wchar_t path[300];
    swprintf(path, 300, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\%hs", argv[1 + unload]);
    USTR u = { (USHORT)(wcslen(path) * 2), (USHORT)(wcslen(path) * 2 + 2), path };
    BOOLEAN was;
    LONG p = RtlAdjustPrivilege(10 /* SeLoadDriverPrivilege */, TRUE, FALSE, &was);
    LONG st = unload ? NtUnloadDriver(&u) : NtLoadDriver(&u);
    printf("%s %s: NTSTATUS 0x%08lx (privilege 0x%08lx)\n", unload ? "unload" : "load", argv[1 + unload], (unsigned long)st, (unsigned long)p);
    return st != 0;
}
