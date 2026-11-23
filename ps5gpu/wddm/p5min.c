/* SPDX-License-Identifier: MIT */
/* The smallest possible driver, to test the clang-cl + lld-link tool chain itself: loads, leaves a mark in its
 * service key (Entry=1), and can be unloaded. */
#include <ntddk.h>
static VOID Unload(PDRIVER_OBJECT d) { UNREFERENCED_PARAMETER(d); }
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path);
void *volatile gSelf = (void *)DriverEntry;      /* an absolute address, so the image has relocations like a real driver */
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path)
{
    OBJECT_ATTRIBUTES oa;
    HANDLE key;
    UNICODE_STRING n;
    ULONG one = 1;
    driver->DriverUnload = Unload;
    InitializeObjectAttributes(&oa, path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&key, KEY_WRITE, &oa))) {
        RtlInitUnicodeString(&n, L"Entry");
        ZwSetValueKey(key, &n, 0, REG_DWORD, &one, sizeof one);
        ZwClose(key);
    }
    return STATUS_SUCCESS;
}
