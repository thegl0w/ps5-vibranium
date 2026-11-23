// SPDX-License-Identifier: MIT
// ps5gpu.sys: kernel shim for the PS5's GPU (PCI 20:00.0, 1002:13FC) under Windows.
//
// What it does: finds the GPU, maps its register BAR, and offers three services to user mode (see ps5gpu_ioctl.h):
// register access through a firewall, mapping of VRAM/doorbell memory, and a log.
//
// What it never does, by construction (SAFETY.md):
//   - It has no PSP or SMU code. The registers of both, and of the thermal, SMUIO, clock and fuse blocks next to them
//     (dword 0x16000-0x17FFF of the register BAR; SMN 0x03800000-0x03BFFFFF and 0x58000-0x5FFFF through an index/data
//     pair; the same bytes through MM_INDEX/MM_DATA) are refused for reads and writes, whoever asks.
//   - It does not touch PCI configuration space except to read it. No bus reset, no power state change.
//   - It does not touch the display: no mode setting, no link training.
//   - It registers no interrupt and does not take the device from Basic Display; it is a plain kernel service.
//   - Until user mode sends the arm magic, every register write is refused (read-only probe).
#include <ddk/ntddk.h>
#include "../include/ps5gpu_ioctl.h"

#ifdef PS5GPU_VM
// Rehearsal in QEMU: the "GPU" is the standard VGA device (00:01.0, 1234:1111). BAR0 = framebuffer, BAR2 = 4 KiB MMIO.
#define GPU_BUS 0
#define GPU_DEV 1
#define GPU_FN 0
#define GPU_ID 0x11111234u
#define GPU_REG_BAR 2
#define GPU_REG_BYTES 0x1000u
#define GPU_BAR0_BYTES 0x1000000ull
#define GPU_BAR2_BYTES 0ull
#else
#define GPU_BUS 0x20
#define GPU_DEV 0
#define GPU_FN 0
#define GPU_ID 0x13FC1002u
#define GPU_REG_BAR 5
#define GPU_REG_BYTES 0x80000u            // 512 KiB (lspci under Linux)
#define GPU_BAR0_BYTES 0x10000000ull      // 256 MiB VRAM window
#define GPU_BAR2_BYTES 0x200000ull        // 2 MiB doorbells
#endif

// Off limits (cyan_skillfish_ip_offset.h): MP0 (PSP) and MP1 (SMU) at dword 0x16000, THM 0x16600, SMUIO 0x16800 and
// 0x16A00, CLK 0x16C00, FUSE 0x17400. The whole block is refused; GPU bring-up needs none of it.
#define MP_REG_FIRST 0x16000u
#define MP_REG_LAST 0x17FFFu
#define SMN_MP_FIRST 0x03800000u          // MP0 and MP1 through SMN
#define SMN_MP_LAST 0x03BFFFFFu
#define SMN_LOW_FIRST 0x00058000u         // the same dword block by its byte address (THM, SMUIO, CLK, FUSE)
#define SMN_LOW_LAST 0x0005FFFFu
#define REG_MM_INDEX 0x0u
#define REG_MM_DATA 0x1u
#define REG_PCIE_INDEX 0xCu
#define REG_PCIE_DATA 0xDu
#define REG_PCIE_INDEX2 0xEu
#define REG_PCIE_DATA2 0xFu

#define FB_LIMIT_LOW 0x100000000ull       // a carve-out mapping must lie above 4 GiB
#define FB_LIMIT_HIGH 0x1000000000ull     // and below 64 GiB
#define MAX_MAPS 4096                       /* 32-bit clients map buffer by buffer (they cannot map the whole heap) */
#define LOG_BYTES 16384

typedef struct MAPPING {
    PVOID UserVa;                   /* base of the view */
    ULONG64 Given;                  /* the address handed to the caller (base + offset inside the first 64 KiB) */
    HANDLE Process;
    ULONG64 Phys, Bytes;
} MAPPING;

static PDEVICE_OBJECT gDevice;
static volatile ULONG *gRegs;
static PS5GPU_INFO gInfo;
static FAST_MUTEX gLock;
static BOOLEAN gArmed;
static MAPPING gMaps[MAX_MAPS];
static CHAR gLog[LOG_BYTES];
static ULONG gLogLen;

static const WCHAR kNtName[] = L"\\Device\\Ps5Gpu";
static const WCHAR kDosName[] = L"\\DosDevices\\Ps5Gpu";

// ---- log (plain text, read through PS5GPU_IOCTL_LOG) ----

static void LogChar(CHAR c)
{
    if (gLogLen < LOG_BYTES - 1) gLog[gLogLen++] = c;
}

static void LogStr(const CHAR *s)
{
    while (*s) LogChar(*s++);
}

static void LogHex(ULONG64 v, int digits)
{
    static const CHAR hex[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; --i) LogChar(hex[(v >> (i * 4)) & 0xF]);
}

static void LogKV(const CHAR *key, ULONG64 v, int digits)
{
    LogStr(key);
    LogStr("0x");
    LogHex(v, digits);
    LogChar(' ');
}

// ---- firewall ----

#ifndef PS5GPU_VM
static BOOLEAN SmnIsMp(ULONG smn)
{
    return (smn >= SMN_MP_FIRST && smn <= SMN_MP_LAST) || (smn >= SMN_LOW_FIRST && smn <= SMN_LOW_LAST);
}

// MM_INDEX with bit 31 clear addresses the register BAR by byte offset.
static BOOLEAN MmIndexIsMp(ULONG index)
{
    return (index & 0x80000000u) == 0 && (index >> 2) >= MP_REG_FIRST && (index >> 2) <= MP_REG_LAST;
}
#endif

// TRUE if the access must not be made. For the data registers the index is read back from the hardware, so an index
// left by someone else (Linux before the hand-over, the firmware) is covered too.
static BOOLEAN Forbidden(ULONG reg, BOOLEAN write, ULONG value)
{
    if ((ULONG64)reg * 4 + 4 > gInfo.RegBytes) return TRUE;
#ifndef PS5GPU_VM
    if (reg >= MP_REG_FIRST && reg <= MP_REG_LAST) return TRUE;
    if (reg == REG_MM_INDEX && write && MmIndexIsMp(value)) return TRUE;
    if (reg == REG_MM_DATA && MmIndexIsMp(gRegs[REG_MM_INDEX])) return TRUE;
    if ((reg == REG_PCIE_INDEX || reg == REG_PCIE_INDEX2) && write && SmnIsMp(value)) return TRUE;
    if (reg == REG_PCIE_DATA && SmnIsMp(gRegs[REG_PCIE_INDEX])) return TRUE;
    if (reg == REG_PCIE_DATA2 && SmnIsMp(gRegs[REG_PCIE_INDEX2])) return TRUE;
#else
    UNREFERENCED_PARAMETER(value);
    UNREFERENCED_PARAMETER(write);
    // The rehearsal keeps one refused register so the path is exercised: dword 0x3FF, the last one.
    if (reg == 0x3FF) return TRUE;
#endif
    if (write && !gArmed) return TRUE;
    return FALSE;
}

static void Refuse(ULONG reg, ULONG value)
{
    gInfo.Refused++;
    gInfo.RefusedLastReg = reg;
    gInfo.RefusedLastValue = value;
}

static void SleepUs(ULONG us)
{
    if (us <= 50) {
        KeStallExecutionProcessor(us);
    } else {
        LARGE_INTEGER t;
        t.QuadPart = -(LONGLONG)us * 10;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
}

// Runs the operations in order. Stops at the first refused or timed-out one; what follows keeps its Op unchanged.
static void RunRegOps(PS5GPU_REGOP *ops, ULONG count)
{
    for (ULONG i = 0; i < count; ++i) {
        PS5GPU_REGOP *op = &ops[i];
        ULONG kind = op->Op & PS5GPU_OP_MASK;
        if (op->Op & ~PS5GPU_OP_MASK) {
            op->Op |= PS5GPU_OP_REFUSED;
            return;
        }
        switch (kind) {
        case PS5GPU_OP_READ:
            if (Forbidden(op->Reg, FALSE, 0)) { Refuse(op->Reg, 0); op->Op |= PS5GPU_OP_REFUSED; return; }
            op->Value = gRegs[op->Reg];
            gInfo.Reads++;
            break;
        case PS5GPU_OP_WRITE: {
            ULONG v = op->Value;
            if (op->Mask != 0) {
                if (Forbidden(op->Reg, FALSE, 0)) { Refuse(op->Reg, 0); op->Op |= PS5GPU_OP_REFUSED; return; }
                v = (gRegs[op->Reg] & ~op->Mask) | (op->Value & op->Mask);
                gInfo.Reads++;
            }
            if (Forbidden(op->Reg, TRUE, v)) { Refuse(op->Reg, v); op->Op |= PS5GPU_OP_REFUSED; return; }
            gRegs[op->Reg] = v;
            gInfo.Writes++;
            break;
        }
        case PS5GPU_OP_DELAY:
            if (op->Value > 100000) { op->Op |= PS5GPU_OP_REFUSED; return; }
            SleepUs(op->Value);
            break;
        case PS5GPU_OP_WAIT: {
            ULONG waited = 0, step = 10, v;
            if (Forbidden(op->Reg, FALSE, 0) || op->Aux > 2000000) { Refuse(op->Reg, 0); op->Op |= PS5GPU_OP_REFUSED; return; }
            for (;;) {
                v = gRegs[op->Reg];
                gInfo.Reads++;
                if ((v & op->Mask) == op->Value) break;
                if (waited >= op->Aux) {
                    op->Value = v;
                    op->Op |= PS5GPU_OP_TIMEOUT;
                    return;
                }
                KeStallExecutionProcessor(step);
                waited += step;
            }
            op->Value = v;
            op->Aux = waited;
            break;
        }
        case PS5GPU_OP_STREAM: {
            ULONG n = op->Aux;
            if (n > count - i - 1) { op->Op |= PS5GPU_OP_REFUSED; return; }
            // The firewall is asked for every value: a stream to an index register must not slip a forbidden index in.
            for (ULONG k = 1; k <= n; ++k) {
                if ((ops[i + k].Op & ~PS5GPU_OP_DONE) != PS5GPU_OP_DATA || Forbidden(op->Reg, TRUE, ops[i + k].Value)) {
                    Refuse(op->Reg, ops[i + k].Value);
                    op->Op |= PS5GPU_OP_REFUSED;
                    return;
                }
            }
            for (ULONG k = 1; k <= n; ++k) {
                gRegs[op->Reg] = ops[i + k].Value;
                ops[i + k].Op |= PS5GPU_OP_DONE;
            }
            gInfo.Writes += n;
            op->Op |= PS5GPU_OP_DONE;
            i += n;
            continue;
        }
        default:
            op->Op |= PS5GPU_OP_REFUSED;
            return;
        }
        op->Op |= PS5GPU_OP_DONE;
    }
}

// ---- memory mapping ----

static BOOLEAN Inside(ULONG64 phys, ULONG64 bytes, ULONG64 base, ULONG64 size)
{
    return size != 0 && phys >= base && bytes <= size && phys - base <= size - bytes;
}

// TRUE if [phys, phys + bytes) shares a page with memory Windows manages as RAM.
static BOOLEAN OverlapsRam(ULONG64 phys, ULONG64 bytes)
{
    BOOLEAN hit = FALSE;
    PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
    if (ranges == NULL) return TRUE;
    for (ULONG i = 0; ranges[i].BaseAddress.QuadPart != 0 || ranges[i].NumberOfBytes.QuadPart != 0; ++i) {
        ULONG64 b = (ULONG64)ranges[i].BaseAddress.QuadPart, n = (ULONG64)ranges[i].NumberOfBytes.QuadPart;
        if (phys < b + n && b < phys + bytes) hit = TRUE;
    }
    ExFreePool(ranges);
    return hit;
}

static NTSTATUS MapUser(PS5GPU_MAP *m)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE section;
    NTSTATUS st;
    PVOID base = NULL;
    SIZE_T view = (SIZE_T)m->Bytes;
    LARGE_INTEGER off;
    ULONG prot, slot;
    BOOLEAN inBar = Inside(m->Phys, m->Bytes, gInfo.Bar[0], GPU_BAR0_BYTES) || Inside(m->Phys, m->Bytes, gInfo.Bar[2], GPU_BAR2_BYTES);

    if (m->Bytes == 0 || (m->Phys & 0xFFF) || (m->Bytes & 0xFFF) || m->Phys + m->Bytes < m->Phys) return STATUS_INVALID_PARAMETER;
    // Never the register BAR: the firewall would be bypassed.
    if (m->Phys < gInfo.Bar[GPU_REG_BAR] + gInfo.RegBytes && gInfo.Bar[GPU_REG_BAR] < m->Phys + m->Bytes) return STATUS_ACCESS_DENIED;
    if (!inBar) {
        if (m->Phys < FB_LIMIT_LOW || m->Phys + m->Bytes > FB_LIMIT_HIGH) return STATUS_ACCESS_DENIED;
        if (OverlapsRam(m->Phys, m->Bytes)) return STATUS_CONFLICTING_ADDRESSES;
    }
    for (slot = 0; slot < MAX_MAPS && gMaps[slot].UserVa != NULL; ++slot) {}
    if (slot == MAX_MAPS) return STATUS_INSUFFICIENT_RESOURCES;

    RtlInitUnicodeString(&name, L"\\Device\\PhysicalMemory");
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    st = ZwOpenSection(&section, SECTION_ALL_ACCESS, &oa);
    if (!NT_SUCCESS(st)) return st;
    off.QuadPart = (LONGLONG)m->Phys;
    prot = PAGE_READWRITE | ((m->Flags & PS5GPU_MAP_WC) ? PAGE_WRITECOMBINE : PAGE_NOCACHE);
    st = ZwMapViewOfSection(section, ZwCurrentProcess(), &base, 0, view, &off, &view, ViewUnmap, 0, prot);
    ZwClose(section);
    if (!NT_SUCCESS(st)) return st;
    // The section rounds the offset down to its allocation granularity; give the caller the address of Phys itself.
    m->UserVa = (ULONG64)(ULONG_PTR)base + (m->Phys - (ULONG64)off.QuadPart);
    gMaps[slot].UserVa = base;
    gMaps[slot].Given = m->UserVa;
    gMaps[slot].Process = PsGetCurrentProcessId();
    gMaps[slot].Phys = m->Phys;
    gMaps[slot].Bytes = m->Bytes;
    gInfo.Maps++;
    if (!inBar && gInfo.FbBytes == 0) {
        gInfo.FbPhys = m->Phys;
        gInfo.FbBytes = m->Bytes;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS UnmapUser(ULONG64 userVa)
{
    for (ULONG slot = 0; slot < MAX_MAPS; ++slot) {
        MAPPING *mp = &gMaps[slot];
        if (mp->UserVa == NULL || mp->Process != PsGetCurrentProcessId()) continue;
        if (userVa != mp->Given) continue;       /* exactly the address MAP returned: views can be adjacent */
        NTSTATUS st = ZwUnmapViewOfSection(ZwCurrentProcess(), mp->UserVa);
        mp->UserVa = NULL;
        gInfo.Maps--;
        return st;
    }
    return STATUS_NOT_FOUND;
}

// A process that exits has its views removed by Windows; forget them here when its handle closes.
static void ForgetProcess(HANDLE pid)
{
    for (ULONG slot = 0; slot < MAX_MAPS; ++slot) {
        if (gMaps[slot].UserVa != NULL && gMaps[slot].Process == pid) {
            ZwUnmapViewOfSection(ZwCurrentProcess(), gMaps[slot].UserVa);
            gMaps[slot].UserVa = NULL;
            gInfo.Maps--;
        }
    }
}

// ---- device ----

static ULONG ConfigRead32(ULONG offset)
{
    PCI_SLOT_NUMBER slot;
    ULONG v = 0xFFFFFFFF;
    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = GPU_DEV;
    slot.u.bits.FunctionNumber = GPU_FN;
    if (HalGetBusDataByOffset(PCIConfiguration, GPU_BUS, slot.u.AsULONG, &v, offset, 4) != 4) return 0xFFFFFFFF;
    return v;
}

static void Probe(void)
{
    ULONG raw[6];
    PHYSICAL_ADDRESS pa;

    gInfo.Version = PS5GPU_VERSION;
    gInfo.Bus = GPU_BUS;
    gInfo.Device = GPU_DEV;
    gInfo.Function = GPU_FN;
#ifdef PS5GPU_VM
    gInfo.Flags |= PS5GPU_F_VM;
#endif
    gInfo.VendorDevice = ConfigRead32(0x00);
    gInfo.CommandStatus = ConfigRead32(0x04);
    gInfo.ClassRev = ConfigRead32(0x08);
    gInfo.Subsystem = ConfigRead32(0x2C);
    LogKV("id ", gInfo.VendorDevice, 8);
    LogKV("cmd ", gInfo.CommandStatus, 8);
    if (gInfo.VendorDevice != GPU_ID) {
        LogStr("not the expected GPU; nothing mapped\n");
        return;
    }
    for (int i = 0; i < 6; ++i) raw[i] = ConfigRead32(0x10 + i * 4);
    for (int i = 0; i < 6; ++i) {
        if (raw[i] & 1) {
            gInfo.Bar[i] = raw[i] & ~3u;                          // I/O
        } else if ((raw[i] & 6) == 4 && i < 5) {
            gInfo.Bar[i] = ((ULONG64)raw[i + 1] << 32) | (raw[i] & ~0xFu);
            ++i;
        } else {
            gInfo.Bar[i] = raw[i] & ~0xFu;
        }
    }
    for (int i = 0; i < 6; ++i) LogKV("bar ", gInfo.Bar[i], 10);
    // Memory decode must be on and the register BAR must be a 32-bit memory BAR with an address.
    if ((gInfo.CommandStatus & 2) == 0 || (raw[GPU_REG_BAR] & 1) || gInfo.Bar[GPU_REG_BAR] == 0) {
        LogStr("register BAR unusable; nothing mapped\n");
        return;
    }
    pa.QuadPart = (LONGLONG)gInfo.Bar[GPU_REG_BAR];
    gRegs = (volatile ULONG *)MmMapIoSpace(pa, GPU_REG_BYTES, MmNonCached);
    if (gRegs == NULL) {
        LogStr("MmMapIoSpace failed\n");
        return;
    }
    gInfo.RegBytes = GPU_REG_BYTES;
    gInfo.Flags |= PS5GPU_F_FOUND;
    LogStr("registers mapped, read-only until armed\n");
}

static NTSTATUS Complete(PIRP irp, NTSTATUS st, ULONG_PTR info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static NTSTATUS NTAPI DispatchCreateClose(PDEVICE_OBJECT dev, PIRP irp)
{
    UNREFERENCED_PARAMETER(dev);
    if (IoGetCurrentIrpStackLocation(irp)->MajorFunction == IRP_MJ_CLEANUP) {
        ExAcquireFastMutex(&gLock);
        ForgetProcess(PsGetCurrentProcessId());
        ExReleaseFastMutex(&gLock);
    }
    return Complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS NTAPI DispatchIoctl(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = irp->AssociatedIrp.SystemBuffer;
    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR done = 0;
    UNREFERENCED_PARAMETER(dev);

    ExAcquireFastMutex(&gLock);
    switch (code) {
    case PS5GPU_IOCTL_INFO:
        if (outLen < sizeof(PS5GPU_INFO)) { st = STATUS_BUFFER_TOO_SMALL; break; }
        gInfo.Flags = (gInfo.Flags & ~PS5GPU_F_ARMED) | (gArmed ? PS5GPU_F_ARMED : 0);
        RtlCopyMemory(buf, &gInfo, sizeof(PS5GPU_INFO));
        done = sizeof(PS5GPU_INFO);
        st = STATUS_SUCCESS;
        break;
    case PS5GPU_IOCTL_REGS: {
        ULONG count = inLen / sizeof(PS5GPU_REGOP);
        if (gRegs == NULL) { st = STATUS_DEVICE_NOT_READY; break; }
        if (count == 0 || inLen % sizeof(PS5GPU_REGOP) || outLen < inLen) { st = STATUS_INVALID_PARAMETER; break; }
        RunRegOps((PS5GPU_REGOP *)buf, count);
        done = inLen;
        st = STATUS_SUCCESS;
        break;
    }
    case PS5GPU_IOCTL_MAP:
        if (gRegs == NULL) { st = STATUS_DEVICE_NOT_READY; break; }
        if (inLen < sizeof(PS5GPU_MAP) || outLen < sizeof(PS5GPU_MAP)) { st = STATUS_INVALID_PARAMETER; break; }
        st = MapUser((PS5GPU_MAP *)buf);
        if (NT_SUCCESS(st)) done = sizeof(PS5GPU_MAP);
        break;
    case PS5GPU_IOCTL_UNMAP:
        if (inLen < sizeof(PS5GPU_MAP)) { st = STATUS_INVALID_PARAMETER; break; }
        st = UnmapUser(((PS5GPU_MAP *)buf)->UserVa);
        break;
    case PS5GPU_IOCTL_ARM:
        if (inLen < sizeof(ULONG)) { st = STATUS_INVALID_PARAMETER; break; }
        if (*(ULONG *)buf == PS5GPU_ARM_MAGIC && gRegs != NULL) {
            gArmed = TRUE;
            LogStr("armed: register writes allowed\n");
            st = STATUS_SUCCESS;
        } else if (*(ULONG *)buf == PS5GPU_DISARM_MAGIC) {
            gArmed = FALSE;
            LogStr("disarmed\n");
            st = STATUS_SUCCESS;
        } else {
            st = STATUS_ACCESS_DENIED;
        }
        break;
    case PS5GPU_IOCTL_LOG: {
        ULONG n = gLogLen < outLen ? gLogLen : outLen;
        RtlCopyMemory(buf, gLog, n);
        done = n;
        st = STATUS_SUCCESS;
        break;
    }
    default:
        break;
    }
    ExReleaseFastMutex(&gLock);
    return Complete(irp, st, done);
}

static VOID NTAPI Unload(PDRIVER_OBJECT drv)
{
    UNICODE_STRING dos;
    UNREFERENCED_PARAMETER(drv);
    RtlInitUnicodeString(&dos, kDosName);
    IoDeleteSymbolicLink(&dos);
    if (gRegs != NULL) MmUnmapIoSpace((PVOID)gRegs, GPU_REG_BYTES);
    if (gDevice != NULL) IoDeleteDevice(gDevice);
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    UNICODE_STRING nt, dos;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(reg);

    ExInitializeFastMutex(&gLock);
    RtlInitUnicodeString(&nt, kNtName);
    RtlInitUnicodeString(&dos, kDosName);
    st = IoCreateDevice(drv, 0, &nt, 0x8000, FILE_DEVICE_SECURE_OPEN, FALSE, &gDevice);
    if (!NT_SUCCESS(st)) return st;
    st = IoCreateSymbolicLink(&dos, &nt);
    if (!NT_SUCCESS(st)) {
        IoDeleteDevice(gDevice);
        return st;
    }
    drv->MajorFunction[IRP_MJ_CREATE] = DispatchCreateClose;
    drv->MajorFunction[IRP_MJ_CLOSE] = DispatchCreateClose;
    drv->MajorFunction[IRP_MJ_CLEANUP] = DispatchCreateClose;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchIoctl;
    drv->DriverUnload = Unload;
    Probe();
    return STATUS_SUCCESS;
}
