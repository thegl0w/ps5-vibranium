/* SPDX-License-Identifier: MIT */
/* devadd <inf> <hardware id>: creates a root-enumerated device with that hardware ID and installs the INF's driver on
 * it (what "devcon install" does). devadd -r <hardware id> removes such devices again. Run elevated. */
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "-r")) {
        HDEVINFO set = SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        SP_DEVINFO_DATA d = { sizeof d };
        int removed = 0;
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
            char ids[512] = { 0 };
            if (SetupDiGetDeviceRegistryPropertyA(set, &d, SPDRP_HARDWAREID, NULL, (BYTE *)ids, sizeof ids - 2, NULL) && !_stricmp(ids, argv[2])) {
                if (SetupDiCallClassInstaller(DIF_REMOVE, set, &d)) ++removed;
                else printf("remove failed: error 0x%lx\n", GetLastError());
            }
        }
        printf("removed %d device(s)\n", removed);
        return 0;
    }
    if (argc != 3) { printf("usage: devadd <inf> <hardware id> | devadd -r <hardware id>\n"); return 2; }
    char inf[MAX_PATH], ids[300] = { 0 }, cls[64];
    GUID guid;
    GetFullPathNameA(argv[1], sizeof inf, inf, NULL);
    snprintf(ids, sizeof ids - 2, "%s", argv[2]);                      /* double-null terminated list */
    if (!SetupDiGetINFClassA(inf, &guid, cls, sizeof cls, NULL)) { printf("cannot read the INF's class: error 0x%lx\n", GetLastError()); return 1; }
    HDEVINFO set = SetupDiCreateDeviceInfoList(&guid, NULL);
    SP_DEVINFO_DATA d = { sizeof d };
    if (!SetupDiCreateDeviceInfoA(set, cls, &guid, NULL, NULL, DICD_GENERATE_ID, &d)) { printf("create device: error 0x%lx\n", GetLastError()); return 1; }
    if (!SetupDiSetDeviceRegistryPropertyA(set, &d, SPDRP_HARDWAREID, (BYTE *)ids, (DWORD)strlen(ids) + 2)) { printf("set hardware id: error 0x%lx\n", GetLastError()); return 1; }
    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &d)) { printf("register device: error 0x%lx\n", GetLastError()); return 1; }
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesA(NULL, argv[2], inf, INSTALLFLAG_FORCE, &reboot)) {
        printf("install driver: error 0x%lx (the new device is removed again)\n", GetLastError());
        SetupDiCallClassInstaller(DIF_REMOVE, set, &d);
        return 1;
    }
    printf("device created and driver installed%s\n", reboot ? " (Windows asks for a restart)" : "");
    return 0;
}
