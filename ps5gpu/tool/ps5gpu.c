// SPDX-License-Identifier: MIT
// ps5gpu.exe: user-mode side of ps5gpu.sys. Stage 1: read-only probe of the GPU as Linux and Windows left it.
//
//   ps5gpu load <path>     create and start the kernel service; ps5gpu unload stops and deletes it
//   ps5gpu info            driver info and log
//   ps5gpu probe           named registers, read only
//   ps5gpu rd <dword> [n]  raw register read
//   ps5gpu vmtest          rehearsal in QEMU (driver built with PS5GPU_VM)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/ps5gpu_ioctl.h"

#include "asic_reg/gc/gc_10_1_0_offset.h"
#include "asic_reg/oss/osssys_5_0_0_offset.h"
#include "asic_reg/nbio/nbio_2_3_offset.h"

// Register block bases for Cyan Skillfish (cyan_skillfish_ip_offset.h), by the header's _BASE_IDX.
#define GC_SEG(i) ((i) == 0 ? 0x1260u : 0xA000u)
#define OSS_SEG(i) (0x10A0u)
#define NBIO_SEG(i) ((i) == 0 ? 0x0u : (i) == 1 ? 0x14u : (i) == 2 ? 0xD20u : 0x10400u)
#define GC(r) (GC_SEG(r##_BASE_IDX) + r)
#define OSS(r) (OSS_SEG(r##_BASE_IDX) + r)
#define NBIO(r) (NBIO_SEG(r##_BASE_IDX) + r)
// Not in the 10.1.0 header; Linux defines them in gfx_v10_0.c (GC segment 1).
#define REG_CP_HYP_PFP_UCODE_ADDR 0xF814u
#define REG_CP_HYP_ME_UCODE_ADDR 0xF816u
#define REG_CP_HYP_CE_UCODE_ADDR 0xF818u
#define REG_CP_MEC_ME1_UCODE_ADDR 0xF81Au
#define REG_CPG_PSP_DEBUG 0xFC10u
#define REG_CPC_PSP_DEBUG 0xFC11u

static HANDLE gDev = INVALID_HANDLE_VALUE;

static int Open(void)
{
    gDev = CreateFileA(PS5GPU_WIN32_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (gDev == INVALID_HANDLE_VALUE) {
        printf("cannot open %s: error %lu (is ps5gpu.sys started?)\n", PS5GPU_WIN32_PATH, GetLastError());
        return 0;
    }
    return 1;
}

static int Ioctl(DWORD code, void *in, DWORD inLen, void *out, DWORD outLen, DWORD *got)
{
    DWORD n = 0;
    BOOL ok = DeviceIoControl(gDev, code, in, inLen, out, outLen, &n, NULL);
    if (got) *got = n;
    if (!ok) printf("ioctl 0x%08lx failed: error %lu\n", code, GetLastError());
    return ok;
}

static int Info(PS5GPU_INFO *info)
{
    DWORD n;
    memset(info, 0, sizeof *info);
    return Ioctl(PS5GPU_IOCTL_INFO, NULL, 0, info, sizeof *info, &n) && n == sizeof *info;
}

static void PrintInfo(void)
{
    PS5GPU_INFO i;
    static char log[16384];
    DWORD n = 0;
    if (!Info(&i)) return;
    printf("driver 0x%08x flags 0x%x (%s%s%s)\n", i.Version, i.Flags, (i.Flags & PS5GPU_F_FOUND) ? "found" : "NOT FOUND",
        (i.Flags & PS5GPU_F_ARMED) ? ", armed" : ", read-only", (i.Flags & PS5GPU_F_VM) ? ", VM rehearsal" : "");
    printf("pci %02x:%02x.%x id %08x cmd/status %08x class/rev %08x subsystem %08x\n", i.Bus, i.Device, i.Function,
        i.VendorDevice, i.CommandStatus, i.ClassRev, i.Subsystem);
    for (int b = 0; b < 6; ++b) printf("bar%d 0x%llx\n", b, (unsigned long long)i.Bar[b]);
    printf("register bytes 0x%x, reads %u, writes %u, refused %u (last reg 0x%x value 0x%x), maps %u\n", i.RegBytes, i.Reads,
        i.Writes, i.Refused, i.RefusedLastReg, i.RefusedLastValue, i.Maps);
    printf("carve-out mapped: 0x%llx + 0x%llx\n", (unsigned long long)i.FbPhys, (unsigned long long)i.FbBytes);
    if (Ioctl(PS5GPU_IOCTL_LOG, NULL, 0, log, sizeof log - 1, &n)) {
        log[n] = 0;
        printf("--- driver log ---\n%s\n", log);
    }
}

// One read. Returns 0 if the driver refused it.
static int Rd(uint32_t reg, uint32_t *value)
{
    PS5GPU_REGOP op = { PS5GPU_OP_READ, reg, 0, 0, 0 };
    DWORD n;
    if (!Ioctl(PS5GPU_IOCTL_REGS, &op, sizeof op, &op, sizeof op, &n)) return 0;
    *value = op.Value;
    return (op.Op & PS5GPU_OP_DONE) != 0;
}

struct Named {
    const char *name;
    uint32_t reg;
    const char *note;
};

#define N(name, reg, note) { name, reg, note }
static const struct Named kProbe[] = {
    N("GRBM_STATUS", GC(mmGRBM_STATUS), "bit 31 = GUI active"),
    N("GRBM_STATUS2", GC(mmGRBM_STATUS2), ""),
    N("CP_STAT", GC(mmCP_STAT), ""),
    N("CP_ME_CNTL", GC(mmCP_ME_CNTL), "halt bits: ME 28, PFP 26, CE 24"),
    N("CP_MEC_CNTL", GC(mmCP_MEC_CNTL), "halt bits: ME1 30, ME2 28"),
    N("RLC_CNTL", GC(mmRLC_CNTL), "bit 0 = RLC enabled"),
    N("RLC_GPM_UCODE_ADDR", GC(mmRLC_GPM_UCODE_ADDR), "Linux leaves the version: 0xd"),
    N("CP_HYP_PFP_UCODE_ADDR", REG_CP_HYP_PFP_UCODE_ADDR, "0x94"),
    N("CP_HYP_CE_UCODE_ADDR", REG_CP_HYP_CE_UCODE_ADDR, "0x25"),
    N("CP_HYP_ME_UCODE_ADDR", REG_CP_HYP_ME_UCODE_ADDR, "0x63"),
    N("CP_MEC_ME1_UCODE_ADDR", REG_CP_MEC_ME1_UCODE_ADDR, "0x90"),
    N("CPG_PSP_DEBUG", REG_CPG_PSP_DEBUG, "bit 3 = GPA override (direct load)"),
    N("CPC_PSP_DEBUG", REG_CPC_PSP_DEBUG, "bit 3"),
    N("CP_PFP_IC_BASE_LO", GC(mmCP_PFP_IC_BASE_LO), "where Linux put PFP's code (its GART: gone now)"),
    N("CP_PFP_IC_BASE_HI", GC(mmCP_PFP_IC_BASE_HI), ""),
    N("CP_PFP_IC_BASE_CNTL", GC(mmCP_PFP_IC_BASE_CNTL), ""),
    N("CP_CE_IC_BASE_LO", GC(mmCP_CE_IC_BASE_LO), ""),
    N("CP_CE_IC_BASE_HI", GC(mmCP_CE_IC_BASE_HI), ""),
    N("CP_ME_IC_BASE_LO", GC(mmCP_ME_IC_BASE_LO), ""),
    N("CP_ME_IC_BASE_HI", GC(mmCP_ME_IC_BASE_HI), ""),
    N("CP_CPC_IC_BASE_LO", GC(mmCP_CPC_IC_BASE_LO), ""),
    N("CP_CPC_IC_BASE_HI", GC(mmCP_CPC_IC_BASE_HI), ""),
    N("CP_RB0_BASE", GC(mmCP_RB0_BASE), "gfx ring"),
    N("CP_RB0_BASE_HI", GC(mmCP_RB0_BASE_HI), ""),
    N("CP_RB0_CNTL", GC(mmCP_RB0_CNTL), ""),
    N("CP_RB0_RPTR", GC(mmCP_RB0_RPTR), ""),
    N("CP_RB0_WPTR", GC(mmCP_RB0_WPTR), ""),
    N("CP_RB_DOORBELL_CONTROL", GC(mmCP_RB_DOORBELL_CONTROL), ""),
    N("SCRATCH_REG0", GC(mmSCRATCH_REG0), ""),
    N("SDMA0_F32_CNTL", GC(mmSDMA0_F32_CNTL), "bit 0 = halt"),
    N("SDMA0_UCODE_ADDR", GC(mmSDMA0_UCODE_ADDR), "0x34"),
    N("SDMA0_STATUS_REG", GC(mmSDMA0_STATUS_REG), ""),
    N("SDMA0_GFX_RB_CNTL", GC(mmSDMA0_GFX_RB_CNTL), ""),
    N("SDMA1_F32_CNTL", GC(mmSDMA1_F32_CNTL), ""),
    N("SDMA1_UCODE_ADDR", GC(mmSDMA1_UCODE_ADDR), "0x34"),
    N("GCMC_VM_FB_LOCATION_BASE", GC(mmGCMC_VM_FB_LOCATION_BASE), "<< 24 = VRAM start in GPU address space (0xF4...)"),
    N("GCMC_VM_FB_LOCATION_TOP", GC(mmGCMC_VM_FB_LOCATION_TOP), ""),
    N("GCMC_VM_FB_OFFSET", GC(mmGCMC_VM_FB_OFFSET), "<< 24 = carve-out physical base"),
    N("GCMC_VM_SYSTEM_APERTURE_LOW", GC(mmGCMC_VM_SYSTEM_APERTURE_LOW_ADDR), ""),
    N("GCMC_VM_SYSTEM_APERTURE_HIGH", GC(mmGCMC_VM_SYSTEM_APERTURE_HIGH_ADDR), ""),
    N("GCMC_VM_MX_L1_TLB_CNTL", GC(mmGCMC_VM_MX_L1_TLB_CNTL), ""),
    N("GCVM_L2_CNTL", GC(mmGCVM_L2_CNTL), ""),
    N("GCVM_CONTEXT0_CNTL", GC(mmGCVM_CONTEXT0_CNTL), ""),
    N("GCVM_CONTEXT0_PT_BASE_LO", GC(mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32), "Linux's GART table"),
    N("GCVM_CONTEXT0_PT_BASE_HI", GC(mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32), ""),
    N("GCVM_CONTEXT0_PT_START_LO", GC(mmGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32), ""),
    N("GCVM_CONTEXT0_PT_END_LO", GC(mmGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32), ""),
    N("GCVM_CONTEXT1_CNTL", GC(mmGCVM_CONTEXT1_CNTL), ""),
    N("IH_RB_CNTL", OSS(mmIH_RB_CNTL), "bit 0 = interrupt ring enabled"),
    N("IH_RB_BASE", OSS(mmIH_RB_BASE), ""),
    N("IH_RB_RPTR", OSS(mmIH_RB_RPTR), ""),
    N("IH_RB_WPTR", OSS(mmIH_RB_WPTR), ""),
    N("RCC_CONFIG_MEMSIZE", NBIO(mmRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE), "VRAM size, MiB"),
};

static int Probe(void)
{
    uint32_t v, fbOffset = 0, memsize = 0;
    PrintInfo();
    printf("--- registers (dword offset, value) ---\n");
    for (size_t i = 0; i < sizeof kProbe / sizeof kProbe[0]; ++i) {
        if (!Rd(kProbe[i].reg, &v)) {
            printf("%-30s 0x%05x REFUSED\n", kProbe[i].name, kProbe[i].reg);
            continue;
        }
        printf("%-30s 0x%05x 0x%08x  %s\n", kProbe[i].name, kProbe[i].reg, v, kProbe[i].note);
        if (kProbe[i].reg == GC(mmGCMC_VM_FB_OFFSET)) fbOffset = v;
        if (kProbe[i].reg == NBIO(mmRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE)) memsize = v;
    }
    printf("carve-out by the registers: physical 0x%llx, %u MiB\n", (unsigned long long)fbOffset << 24, memsize);
    // The firewall must hold: one read inside the PSP/SMU block has to come back refused.
    printf("firewall check, read of dword 0x16080 (PSP mailbox): %s\n", Rd(0x16080, &v) ? "NOT REFUSED (bug)" : "refused, as it must be");
    return 0;
}

static int VmTest(void)
{
    PS5GPU_INFO i;
    PS5GPU_REGOP ops[4];
    PS5GPU_MAP m;
    DWORD n;
    uint32_t v = 0, magic, xres;
    int bad = 0;

    if (!Info(&i)) return 1;
    PrintInfo();
    if (!(i.Flags & PS5GPU_F_VM) || !(i.Flags & PS5GPU_F_FOUND)) { printf("VMTEST: not the rehearsal driver, or no VGA device\n"); return 1; }
    // QEMU VGA MMIO BAR: Bochs dispi registers at byte 0x500 (16-bit each): index 0 = ID (0xB0Cx).
    if (!Rd(0x140, &v) || (v & 0xFFF0) != 0xB0C0) { printf("VMTEST FAIL: dispi ID read 0x%08x\n", v); bad = 1; } else printf("VMTEST ok: dispi id 0x%04x, xres %u\n", v & 0xFFFF, v >> 16);
    xres = v >> 16;
    if (Rd(0x3FF, &v)) { printf("VMTEST FAIL: refused register was read\n"); bad = 1; } else printf("VMTEST ok: firewall refused the read\n");
    // Write before arming must be refused. Byte 0x604 = byte-order register; writing its own value back changes nothing.
    memset(ops, 0, sizeof ops);
    ops[0].Op = PS5GPU_OP_READ; ops[0].Reg = 0x181;
    ops[1].Op = PS5GPU_OP_WRITE; ops[1].Reg = 0x181; ops[1].Value = 0x1e1e1e1e;
    Ioctl(PS5GPU_IOCTL_REGS, ops, 2 * sizeof ops[0], ops, 2 * sizeof ops[0], &n);
    if (!(ops[1].Op & PS5GPU_OP_REFUSED)) { printf("VMTEST FAIL: write before arm went through\n"); bad = 1; } else printf("VMTEST ok: write refused before arm (byte order reg 0x%08x)\n", ops[0].Value);
    magic = PS5GPU_ARM_MAGIC;
    Ioctl(PS5GPU_IOCTL_ARM, &magic, sizeof magic, NULL, 0, &n);
    memset(ops, 0, sizeof ops);
    ops[0].Op = PS5GPU_OP_WRITE; ops[0].Reg = 0x181; ops[0].Value = 0x1e1e1e1e;
    ops[1].Op = PS5GPU_OP_WAIT; ops[1].Reg = 0x181; ops[1].Mask = 0xFFFFFFFF; ops[1].Value = 0x1e1e1e1e; ops[1].Aux = 1000;
    ops[2].Op = PS5GPU_OP_DELAY; ops[2].Value = 200;
    ops[3].Op = PS5GPU_OP_WAIT; ops[3].Reg = 0x181; ops[3].Mask = 0xFFFFFFFF; ops[3].Value = 0x12345678; ops[3].Aux = 500;
    Ioctl(PS5GPU_IOCTL_REGS, ops, sizeof ops, ops, sizeof ops, &n);
    if (!(ops[0].Op & PS5GPU_OP_DONE) || !(ops[1].Op & PS5GPU_OP_DONE) || !(ops[2].Op & PS5GPU_OP_DONE) || !(ops[3].Op & PS5GPU_OP_TIMEOUT)) {
        printf("VMTEST FAIL: armed sequence: %08x %08x %08x %08x\n", ops[0].Op, ops[1].Op, ops[2].Op, ops[3].Op);
        bad = 1;
    } else printf("VMTEST ok: write, wait, delay, timeout\n");
    // Map the framebuffer BAR and draw a block; the screenshot shows it.
    memset(&m, 0, sizeof m);
    m.Phys = i.Bar[0]; m.Bytes = 0x400000; m.Flags = PS5GPU_MAP_WC;
    if (!Ioctl(PS5GPU_IOCTL_MAP, &m, sizeof m, &m, sizeof m, &n) || m.UserVa == 0) { printf("VMTEST FAIL: map of BAR0\n"); bad = 1; }
    else {
        uint32_t *fb = (uint32_t *)(uintptr_t)m.UserVa;
        if (xres == 0 || xres > 4096) xres = 1024;
        for (uint32_t y = 100; y < 200; ++y) for (uint32_t x = 100; x < 400; ++x) fb[y * xres + x] = 0x0000FF00;
        printf("VMTEST ok: mapped BAR0 at %p, pixel reads back 0x%08x\n", (void *)fb, fb[150 * xres + 150]);
        if (fb[150 * xres + 150] != 0x0000FF00) bad = 1;
        Ioctl(PS5GPU_IOCTL_UNMAP, &m, sizeof m, NULL, 0, &n);
    }
    // RAM must never be mappable: 0x100000000 is RAM when the VM has more than 3 GiB.
    memset(&m, 0, sizeof m);
    m.Phys = 0x100000000ull; m.Bytes = 0x10000; m.Flags = PS5GPU_MAP_WC;
    if (DeviceIoControl(gDev, PS5GPU_IOCTL_MAP, &m, sizeof m, &m, sizeof m, &n, NULL)) { printf("VMTEST FAIL: RAM was mapped\n"); bad = 1; }
    else printf("VMTEST ok: RAM mapping refused, error %lu\n", GetLastError());
    // Nor the register BAR.
    memset(&m, 0, sizeof m);
    m.Phys = i.Bar[2]; m.Bytes = 0x1000;
    if (DeviceIoControl(gDev, PS5GPU_IOCTL_MAP, &m, sizeof m, &m, sizeof m, &n, NULL)) { printf("VMTEST FAIL: register BAR was mapped\n"); bad = 1; }
    else printf("VMTEST ok: register BAR mapping refused, error %lu\n", GetLastError());
    PrintInfo();
    printf(bad ? "VMTEST RESULT: FAIL\n" : "VMTEST RESULT: PASS\n");
    return bad;
}

// Service control: create and start (or stop and delete) the kernel service. Prints Windows' error code on failure:
// 577 = signature not accepted (test-signing off), 2 = file not found, 1275 = blocked by policy.
static int Load(const char *path)
{
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS), svc;
    int rc = 1;
    if (!scm) { printf("OpenSCManager failed: error %lu\n", GetLastError()); return 1; }
    svc = CreateServiceA(scm, "ps5gpu", "PS5 GPU shim", SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
        SERVICE_ERROR_NORMAL, path, NULL, NULL, NULL, NULL, NULL);
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) svc = OpenServiceA(scm, "ps5gpu", SERVICE_ALL_ACCESS);
    if (!svc) { printf("CreateService failed: error %lu\n", GetLastError()); CloseServiceHandle(scm); return 1; }
    if (StartServiceA(svc, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
        printf("ps5gpu service running\n");
        rc = 0;
    } else {
        printf("StartService failed: error %lu\n", GetLastError());
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return rc;
}

static int Unload(void)
{
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS), svc;
    SERVICE_STATUS st;
    if (!scm) { printf("OpenSCManager failed: error %lu\n", GetLastError()); return 1; }
    svc = OpenServiceA(scm, "ps5gpu", SERVICE_ALL_ACCESS);
    if (!svc) { printf("OpenService failed: error %lu\n", GetLastError()); CloseServiceHandle(scm); return 1; }
    if (ControlService(svc, SERVICE_CONTROL_STOP, &st)) printf("stopped\n"); else printf("stop: error %lu\n", GetLastError());
    if (DeleteService(svc)) printf("service deleted\n"); else printf("delete: error %lu\n", GetLastError());
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "info";
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(cmd, "load") && argc > 2) return Load(argv[2]);
    if (!strcmp(cmd, "unload")) return Unload();
    if (!Open()) return 2;
    if (!strcmp(cmd, "info")) { PrintInfo(); return 0; }
    if (!strcmp(cmd, "probe")) return Probe();
    if (!strcmp(cmd, "vmtest")) return VmTest();
    if (!strcmp(cmd, "rd") && argc > 2) {
        uint32_t reg = strtoul(argv[2], NULL, 0), n = argc > 3 ? strtoul(argv[3], NULL, 0) : 1, v;
        for (uint32_t k = 0; k < n; ++k) {
            if (Rd(reg + k, &v)) printf("0x%05x 0x%08x\n", reg + k, v); else printf("0x%05x REFUSED\n", reg + k);
        }
        return 0;
    }
    printf("usage: ps5gpu load <full path of ps5gpu.sys> | unload | info | probe | rd <dword> [count] | vmtest\n");
    return 2;
}
