// SPDX-License-Identifier: MIT
// Stage 2: take over the PS5's GPU under Windows, in user mode, through ps5gpu.sys.
//
//   ps5gpu2 look                      read-only: where everything is, what Linux left
//   ps5gpu2 run <fwdir> [upto]        steps 1..upto (default: all):
//                                       1 interrupt ring off, engines halted (quiesce)
//                                       2 stale GART table emptied, fault page moved into VRAM
//                                       3 RLC: microcode loaded directly, clear-state buffer, start
//                                       4 CP and SDMA microcode loaded directly (CP's code in VRAM)
//                                       5 graphics ring started
//                                       6 ring test (the CP writes a register for us)
//                                       7 the GPU fills a rectangle on the screen
//                                       8 a command buffer in its own address space (VMID 1), with a fence
//   ps5gpu2 fill <x> <y> <w> <h> <rgb>   another rectangle, once step 5 has run in this process... (see main)
//
// Follows Linux's amdgpu for this chip (gfx_v10_0.c, gfxhub_v2_0.c, navi10_ih.c; Linux 7.2 + the PS5 patch) with
// firmware.load_type = DIRECT. The microcode load itself is CyanDirectLoad (cleanroom/directload-mit, MIT, written from a
// facts-only spec; it replaced an earlier non-commercial implementation). Nothing here touches the PSP, the SMU, the display or PCI configuration space;
// RLC_PG_CNTL bit 23 is set so that the RLC sends no message to the SMU either.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "backend.h"
#include "../lib/ps5drm.h"
#include "CyanDirectLoad.hpp"

#include "regs.h"
#include "layout.h"
#include "clearstate_defs.h"
#include "clearstate_gfx10.h"

using namespace p5layout;

struct Ctx {
    uint64_t carvePhys = 0, mcBase = 0, vramBytes = 0;
    uint64_t areaOff = 0;
    uint8_t* area = nullptr;
    uint64_t surfMc = 0;
    uint32_t pitchPx = 0;
    uint32_t* ring = nullptr;
    uint32_t ringDw = (uint32_t)(kRingBytes / 4);
    uint64_t wptr = 0;
    uint64_t areaMc() const { return mcBase + areaOff; }
    uint64_t areaPhys() const { return carvePhys + areaOff; }
};

static bool gFail;

static uint32_t R(uint32_t reg)
{
    uint32_t v = 0;
    if (!be::rd(reg, v)) {
        printf("  REFUSED read 0x%05x\n", reg);
        gFail = true;
        return 0xFFFFFFFF;
    }
    return v;
}

static void W(uint32_t reg, uint32_t value)
{
    if (!be::wr(reg, value)) {
        printf("  REFUSED write 0x%05x = 0x%08x\n", reg, value);
        gFail = true;
    }
}

static uint32_t ioRead(void*, uint32_t reg) { return R(reg); }
static void ioWrite(void*, uint32_t reg, uint32_t value) { W(reg, value); }
static void ioDelay(void*, uint32_t us) { be::delayUs(us); }
static const CyanDirectLoad::Io kIo = { nullptr, ioRead, ioWrite, ioDelay };

// Polls until (reg & mask) == want. Returns the microseconds it took, or -1.
static int64_t waitReg(uint32_t reg, uint32_t mask, uint32_t want, uint32_t timeoutUs, uint32_t* last = nullptr)
{
    uint64_t start = be::nowUs();
    for (;;) {
        uint32_t v = R(reg);
        if (last) *last = v;
        if (gFail) return -1;
        if ((v & mask) == want) return (int64_t)(be::nowUs() - start);
        if (be::nowUs() - start > timeoutUs) return -1;
        be::delayUs(10);
    }
}

// ---- where things are ----

static bool discover(Ctx& c, bool verbose)
{
    uint32_t fbBase = R(GC(mmGCMC_VM_FB_LOCATION_BASE)), fbTop = R(GC(mmGCMC_VM_FB_LOCATION_TOP));
    uint32_t fbOffset = R(GC(mmGCMC_VM_FB_OFFSET)), memsize = R(NBIO(mmRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE));
    uint32_t surfLo = R(DMU(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS)), surfHi = R(DMU(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH));
    uint32_t pitch = R(DMU(mmHUBPREQ0_DCSURF_SURFACE_PITCH));
    if (gFail) return false;
    c.mcBase = (uint64_t)(fbBase & 0xFFFFFF) << 24;
    uint64_t mcTop = ((uint64_t)(fbTop & 0xFFFFFF) << 24) | 0xFFFFFF;
    c.vramBytes = mcTop - c.mcBase + 1;
    c.carvePhys = (uint64_t)fbOffset << 24;
    // The display takes the surface address as a physical address on this machine (Linux: 0x4_65000000 for a buffer at
    // VRAM offset 0xF5000000). Accept a GPU address too. Either way we keep the GPU (MC) address of the same memory.
    uint64_t surfRaw = ((uint64_t)(surfHi & 0xFFFF) << 32) | surfLo;
    c.pitchPx = (pitch & 0x3FFF) + 1;
    if (verbose) {
        printf("VRAM: GPU address 0x%llx..0x%llx (%llu MiB), physical 0x%llx, CONFIG_MEMSIZE %u MiB\n", (unsigned long long)c.mcBase,
            (unsigned long long)mcTop, (unsigned long long)(c.vramBytes >> 20), (unsigned long long)c.carvePhys, memsize);
    }
    if (c.carvePhys < 0x100000000ull || c.vramBytes < 0x40000000ull || c.vramBytes > 0x400000000ull || mcTop < c.mcBase) {
        printf("STOP: VRAM layout not as expected (need a carve-out above 4 GiB of at least 1 GiB)\n");
        return false;
    }
    if ((uint64_t)memsize << 20 != c.vramBytes) printf("note: CONFIG_MEMSIZE and the FB location disagree\n");
    c.areaOff = c.vramBytes - kAreaBelowTop;
    if (surfRaw >= c.carvePhys && surfRaw < c.carvePhys + c.vramBytes) c.surfMc = c.mcBase + (surfRaw - c.carvePhys);
    else c.surfMc = surfRaw;
    if (verbose) printf("scan-out register: 0x%llx (%s)\n", (unsigned long long)surfRaw,
        surfRaw >= c.carvePhys && surfRaw < c.carvePhys + c.vramBytes ? "a physical address in the carve-out" : "taken as a GPU address");
    if (verbose) printf("scan-out: GPU address 0x%llx (VRAM offset 0x%llx), pitch %u pixels (raw 0x%08x)\n", (unsigned long long)c.surfMc,
        (unsigned long long)(c.surfMc - c.mcBase), c.pitchPx, pitch);
    bool surfOk = c.surfMc >= c.mcBase && c.surfMc + (uint64_t)c.pitchPx * 4 * 2160 <= c.mcBase + c.vramBytes && c.pitchPx >= 640 && c.pitchPx <= 8192;
    if (!surfOk) {
        printf("note: scan-out registers not usable; no drawing will be attempted\n");
        c.surfMc = 0;
    } else if (c.surfMc + (uint64_t)c.pitchPx * 4 * 2160 > c.areaMc() && c.surfMc < c.areaMc() + kAreaBytes) {
        printf("STOP: the scan-out image overlaps our VRAM area\n");
        return false;
    }
    if (verbose) printf("our VRAM area: offset 0x%llx, GPU address 0x%llx, physical 0x%llx, %llu MiB\n", (unsigned long long)c.areaOff,
        (unsigned long long)c.areaMc(), (unsigned long long)c.areaPhys(), (unsigned long long)(kAreaBytes >> 20));
    return true;
}

static void look(Ctx& c)
{
    CyanDirectLoad::Snapshot s;
    discover(c, true);
    CyanDirectLoad::snapshot(kIo, s);
    printf("RLC_CNTL 0x%08x  RLC ucode addr 0x%x  GRBM_STATUS 0x%08x\n", s.rlcCntl, s.rlcGpmUcodeAddr, s.grbmStatus);
    printf("CP_ME_CNTL 0x%08x  CP_MEC_CNTL 0x%08x  CP_STAT 0x%08x  PSP_DEBUG cpg 0x%x cpc 0x%x\n", s.cpMeCntl, s.cpMecCntl, s.cpStat, s.cpgPspDebug, s.cpcPspDebug);
    printf("IC_BASE pfp %08x:%08x ce %08x:%08x me %08x:%08x mec %08x:%08x\n", s.pfpIcBaseHi, s.pfpIcBaseLo, s.ceIcBaseHi, s.ceIcBaseLo,
        s.meIcBaseHi, s.meIcBaseLo, s.cpcIcBaseHi, s.cpcIcBaseLo);
    for (int i = 0; i < 2; ++i) printf("SDMA%d F32_CNTL 0x%08x CNTL 0x%08x STATUS 0x%08x RB_CNTL 0x%08x\n", i, s.sdmaF32Cntl[i], s.sdmaCntl[i], s.sdmaStatus[i], s.sdmaRbCntl[i]);
    printf("RLC_PG_CNTL 0x%08x  RLC_CGCG_CGLS_CTRL 0x%08x  RLC_CSIB %08x:%08x len 0x%x  RLC_SPM_MC_CNTL 0x%08x\n", R(GC(mmRLC_PG_CNTL)),
        R(GC(mmRLC_CGCG_CGLS_CTRL)), R(GC(mmRLC_CSIB_ADDR_HI)), R(GC(mmRLC_CSIB_ADDR_LO)), R(GC(mmRLC_CSIB_LENGTH)), R(GC(mmRLC_SPM_MC_CNTL)));
    printf("VMID0 page table base %08x:%08x start 0x%08x end 0x%08x cntl 0x%08x\n", R(GC(mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32)),
        R(GC(mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32)), R(GC(mmGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32)),
        R(GC(mmGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32)), R(GC(mmGCVM_CONTEXT0_CNTL)));
    printf("fault default page %08x:%08x (<<12)  system aperture default %08x:%08x (<<12)  fault status 0x%08x\n",
        R(GC(mmGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32)), R(GC(mmGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32)),
        R(GC(mmGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB)), R(GC(mmGCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB)), R(GC(mmGCVM_L2_PROTECTION_FAULT_STATUS)));
    printf("system aperture low 0x%08x high 0x%08x (<<18)\n", R(GC(mmGCMC_VM_SYSTEM_APERTURE_LOW_ADDR)), R(GC(mmGCMC_VM_SYSTEM_APERTURE_HIGH_ADDR)));
    printf("gfx ring: base %08x:%08x cntl 0x%08x rptr 0x%x wptr 0x%x doorbell 0x%08x\n", R(GC(mmCP_RB0_BASE_HI)), R(GC(mmCP_RB0_BASE)),
        R(GC(mmCP_RB0_CNTL)), R(GC(mmCP_RB0_RPTR)), R(GC(mmCP_RB0_WPTR)), R(GC(mmCP_RB_DOORBELL_CONTROL)));
    // Display link, read only (nothing here ever writes a display register). On 2026-11-19 a launch handed Windows a
    // dead link: DP_LINK_CNTL 0 (bit 4 = training complete), DIG_BE_EN 0x100 (bit 0 = enabled), monitor "no signal".
    uint32_t f0 = R(DMU(mmOTG0_OTG_STATUS_FRAME_COUNT));
    printf("display: DP_LINK_CNTL 0x%x DIG_BE_EN 0x%x DIG_FE 0x%x VID_STREAM 0x%08x OTG_CONTROL 0x%08x frame 0x%x\n", R(DMU(mmDP0_DP_LINK_CNTL)),
        R(DMU(mmDIG0_DIG_BE_EN_CNTL)), R(DMU(mmDIG0_DIG_FE_CNTL)), R(DMU(mmDP0_DP_VID_STREAM_CNTL)), R(DMU(mmOTG0_OTG_CONTROL)), f0);
    uint32_t w0 = R(OSS(mmIH_RB_WPTR));
    printf("interrupt ring: cntl 0x%08x base 0x%08x rptr 0x%x wptr 0x%x", R(OSS(mmIH_RB_CNTL)), R(OSS(mmIH_RB_BASE)), R(OSS(mmIH_RB_RPTR)), w0);
    be::delayUs(1000000);
    uint32_t w1 = R(OSS(mmIH_RB_WPTR));
    printf(", wptr one second later 0x%x (%s)\n", w1, w1 == w0 ? "quiet" : "THE GPU IS WRITING INTERRUPT ENTRIES INTO OLD LINUX MEMORY");
}

// ---- step 1: interrupt ring off, engines halted ----

static uint32_t gOurIhBase;      // IH_RB_BASE value of ps5wddm's interrupt ring (area + 0x40000), set once the area is known

static void stepQuiesce()
{
    // navi10_ih: ring and interrupt delivery off, pointers cleared. Ring 0 lives in memory that belonged to Linux.
    uint32_t t = R(OSS(mmIH_RB_CNTL));
    if (gOurIhBase && R(OSS(mmIH_RB_BASE)) == gOurIhBase && (t & IH_RB_CNTL__RB_ENABLE_MASK)) {
        printf("step 1: interrupt ring 0 is the display driver's own (in our area): left running\n");
    } else {
        W(OSS(mmIH_RB_CNTL), t & ~(IH_RB_CNTL__RB_ENABLE_MASK | IH_RB_CNTL__ENABLE_INTR_MASK));
        W(OSS(mmIH_RB_RPTR), 0);
        W(OSS(mmIH_RB_WPTR), 0);
    }
    t = R(OSS(mmIH_RB_CNTL_RING1));
    W(OSS(mmIH_RB_CNTL_RING1), t & ~IH_RB_CNTL_RING1__RB_ENABLE_MASK);
    t = R(OSS(mmIH_RB_CNTL_RING2));
    W(OSS(mmIH_RB_CNTL_RING2), t & ~IH_RB_CNTL_RING2__RB_ENABLE_MASK);
    printf("step 1: interrupt rings off (IH_RB_CNTL now 0x%08x)\n", R(OSS(mmIH_RB_CNTL)));
    CyanDirectLoad::quiesce(kIo, true);
    printf("step 1: CP_ME_CNTL 0x%08x CP_MEC_CNTL 0x%08x SDMA0/1 F32_CNTL 0x%08x 0x%08x (all halted)\n", R(GC(mmCP_ME_CNTL)), R(GC(mmCP_MEC_CNTL)),
        R(GC(mmSDMA0_F32_CNTL)), R(GC(mmSDMA1_F32_CNTL)));
    // A CP that hung in the middle of a packet stays busy through a halt and a microcode reload (seen 2026-11-19 after
    // a packet straddled the ring end: CP_STAT 0x94008200, PFP_BUSY, ROQ_RING_BUSY). Linux's answer to "CP busy, the
    // rest of the pipeline idle" is gfx_v10_0_soft_reset: RLC stopped, CP halted, GRBM_SOFT_RESET.SOFT_RESET_CP pulsed
    // for 50 us. It is an engine soft reset through the same register the SDMA reset above uses; it is not the bus
    // reset or full GPU reset of SAFETY.md. Everything it clears is programmed again by steps 3 to 5.
    uint32_t stat = 0;
    if (waitReg(GC(mmCP_STAT), 0xFFFFFFFF, 0, 100000, &stat) < 0 && !gFail) {
        printf("step 1: CP still busy while halted (CP_STAT 0x%08x, GRBM_STATUS 0x%08x): CP soft reset\n", stat, R(GC(mmGRBM_STATUS)));
        W(GC(mmRLC_CNTL), R(GC(mmRLC_CNTL)) & ~RLC_CNTL__RLC_ENABLE_F32_MASK);
        uint32_t t2 = R(GC(mmGRBM_SOFT_RESET));
        W(GC(mmGRBM_SOFT_RESET), t2 | GRBM_SOFT_RESET__SOFT_RESET_CP_MASK);
        R(GC(mmGRBM_SOFT_RESET));
        be::delayUs(50);
        W(GC(mmGRBM_SOFT_RESET), t2 & ~GRBM_SOFT_RESET__SOFT_RESET_CP_MASK);
        R(GC(mmGRBM_SOFT_RESET));
        be::delayUs(50);
        printf("step 1: after the CP soft reset: CP_STAT 0x%08x, GRBM_STATUS 0x%08x, CP_ME_CNTL 0x%08x\n", R(GC(mmCP_STAT)), R(GC(mmGRBM_STATUS)), R(GC(mmCP_ME_CNTL)));
        // A shader that hung (seen 2026-11-19: a compute shader reading address 0) leaves the pipeline busy through the
        // CP reset (GRBM_STATUS 0x80403028, SPI_BUSY), and every later command buffer then waits forever. For busy
        // pipeline blocks gfx_v10_0_soft_reset pulses SOFT_RESET_CP | SOFT_RESET_GFX. Still an engine soft reset.
        const uint32_t pipelineBusy = GRBM_STATUS__PA_BUSY_MASK | GRBM_STATUS__SC_BUSY_MASK | GRBM_STATUS__BCI_BUSY_MASK | GRBM_STATUS__SX_BUSY_MASK |
                                      GRBM_STATUS__TA_BUSY_MASK | GRBM_STATUS__DB_BUSY_MASK | GRBM_STATUS__CB_BUSY_MASK | GRBM_STATUS__GDS_BUSY_MASK |
                                      GRBM_STATUS__SPI_BUSY_MASK | GRBM_STATUS__GE_BUSY_NO_DMA_MASK;
        uint32_t gs = R(GC(mmGRBM_STATUS));
        if (gs & pipelineBusy) {
            const uint32_t bits = GRBM_SOFT_RESET__SOFT_RESET_CP_MASK | GRBM_SOFT_RESET__SOFT_RESET_GFX_MASK;
            printf("step 1: pipeline still busy (GRBM_STATUS 0x%08x): CP + GFX soft reset\n", gs);
            t2 = R(GC(mmGRBM_SOFT_RESET));
            W(GC(mmGRBM_SOFT_RESET), t2 | bits);
            R(GC(mmGRBM_SOFT_RESET));
            be::delayUs(50);
            W(GC(mmGRBM_SOFT_RESET), t2 & ~bits);
            R(GC(mmGRBM_SOFT_RESET));
            be::delayUs(50);
            printf("step 1: after the CP + GFX soft reset: CP_STAT 0x%08x, GRBM_STATUS 0x%08x\n", R(GC(mmCP_STAT)), R(GC(mmGRBM_STATUS)));
        }
        // The reset may clear the halt bits; keep everything halted until the microcode is back.
        W(GC(mmCP_ME_CNTL), R(GC(mmCP_ME_CNTL)) | CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK | CP_ME_CNTL__CE_HALT_MASK);
        W(GC(mmCP_MEC_CNTL), R(GC(mmCP_MEC_CNTL)) | CP_MEC_CNTL__MEC_ME1_HALT_MASK | CP_MEC_CNTL__MEC_ME2_HALT_MASK);
    }
}

// ---- step 2: nothing may reach old Linux memory ----

// gmc_v10_0_flush_gpu_tlb, the register path (no KIQ), engine 17, as for the GART.
static bool flushTlb(uint32_t vmid)
{
    uint32_t req = 0;
    req |= (1u << vmid) << GCVM_INVALIDATE_ENG0_REQ__PER_VMID_INVALIDATE_REQ__SHIFT;
    req |= GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PTES_MASK | GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE0_MASK |
           GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE1_MASK | GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE2_MASK |
           GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L1_PTES_MASK;
    W(GC(mmGCVM_INVALIDATE_ENG17_REQ), req);
    R(GC(mmGCVM_INVALIDATE_ENG17_REQ));
    return waitReg(GC(mmGCVM_INVALIDATE_ENG17_ACK), 1u << vmid, 1u << vmid, 100000) >= 0;
}

static bool stepGart(Ctx& c)
{
    uint64_t ptBase = (((uint64_t)R(GC(mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32)) << 32) | R(GC(mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32))) & ~0xFFFull;
    uint64_t start = R(GC(mmGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32)), end = R(GC(mmGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32));
    uint64_t bytes = (end - start + 1) * 8;
    // The register holds the table's physical address (Linux: 0x4_6FF00001 = carve-out + 0xFFF00000, valid bit set).
    if (ptBase < c.carvePhys || ptBase + bytes > c.carvePhys + c.vramBytes || bytes == 0 || bytes > 0x1000000 || end < start) {
        printf("STOP step 2: VMID 0 page table 0x%llx (+0x%llx) is not inside the carve-out\n", (unsigned long long)ptBase, (unsigned long long)bytes);
        return false;
    }
    if (ptBase + bytes > c.areaPhys() && ptBase < c.areaPhys() + kAreaBytes) {
        printf("STOP step 2: Linux's page table overlaps our VRAM area\n");
        return false;
    }
    // Linux's GART table still maps GPU addresses 0..512 MiB to pages that are Windows' RAM now. Empty it.
    uint64_t tablePhys = ptBase, mapPhys = tablePhys & ~0xFFFFull;
    uint64_t mapBytes = ((tablePhys + bytes + 0xFFFF) & ~0xFFFFull) - mapPhys;
    uint8_t* map = (uint8_t*)be::map(mapPhys, mapBytes, true);
    if (!map) return false;
    uint64_t* table = (uint64_t*)(map + (tablePhys - mapPhys));
    uint64_t valid = 0, first = 0;
    for (uint64_t i = 0; i < bytes / 8; ++i) {
        if (table[i] & 1) {
            if (!valid) first = table[i];
            ++valid;
        }
        table[i] = 0;
    }
    be::flushWrites();
    printf("step 2: GART table at physical 0x%llx, %llu entries, %llu were valid (first 0x%llx); emptied\n", (unsigned long long)ptBase,
        (unsigned long long)(bytes / 8), (unsigned long long)valid, (unsigned long long)first);
    // A faulting access is redirected to this page. Linux pointed it at a page of its own RAM.
    uint64_t dummyPhys = c.areaPhys() + kDummyOff;
    W(GC(mmGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32), (uint32_t)(dummyPhys >> 12));
    W(GC(mmGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32), (uint32_t)(dummyPhys >> 44));
    bool ok = flushTlb(0);
    printf("step 2: fault page now physical 0x%llx; VMID 0 TLB flush %s\n", (unsigned long long)dummyPhys, ok ? "acknowledged" : "NOT acknowledged");
    return ok;
}

// ---- firmware files ----

struct Firmware {
    std::vector<uint8_t> file[7];
    CyanDirectLoad::Image rlc, sdma0, sdma1;
    CyanDirectLoad::CpImages cp;
};

static bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = n > 0 && fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

static bool loadFirmware(const std::string& dir, Firmware& fw)
{
    using CyanDirectLoad::Kind;
    struct { const char* name; Kind kind; CyanDirectLoad::Image* image; } list[7] = {
        { "cyan_skillfish2_rlc.bin", Kind::Rlc, &fw.rlc },     { "cyan_skillfish2_pfp.bin", Kind::CpGfx, &fw.cp.pfp },
        { "cyan_skillfish2_ce.bin", Kind::CpGfx, &fw.cp.ce },  { "cyan_skillfish2_me.bin", Kind::CpGfx, &fw.cp.me },
        { "cyan_skillfish2_mec.bin", Kind::CpMec, &fw.cp.mec }, { "cyan_skillfish2_sdma.bin", Kind::Sdma, &fw.sdma0 },
        { "cyan_skillfish2_sdma1.bin", Kind::Sdma, &fw.sdma1 },
    };
    for (int i = 0; i < 7; ++i) {
        std::string path = dir + "/" + list[i].name;
        if (!readFile(path, fw.file[i]) || !CyanDirectLoad::parse(fw.file[i].data(), fw.file[i].size(), list[i].kind, *list[i].image)) {
            printf("STOP: firmware file %s missing or not as expected\n", path.c_str());
            return false;
        }
        printf("  %s: version 0x%x, %u bytes of code\n", list[i].name, list[i].image->version, list[i].image->codeBytes);
    }
    return true;
}

// ---- step 3: RLC ----

// gfx_v10_0_get_csb_buffer (amdgpu_gfx_csb_preamble_start, _data_parser, PA_SC_TILE_STEERING_OVERRIDE = 0, _preamble_end).
static uint32_t buildCsb(uint32_t* b)
{
    uint32_t n = 0;
    b[n++] = PACKET3(PACKET3_PREAMBLE_CNTL, 0);
    b[n++] = PACKET3_PREAMBLE_BEGIN_CLEAR_STATE;
    b[n++] = PACKET3(PACKET3_CONTEXT_CONTROL, 1);
    b[n++] = 0x80000000;
    b[n++] = 0x80000000;
    for (const cs_section_def* sect = gfx10_cs_data; sect->section != NULL; ++sect) {
        for (const cs_extent_def* ext = sect->section; ext->extent != NULL; ++ext) {
            if (sect->id != SECT_CONTEXT) continue;
            b[n++] = PACKET3(PACKET3_SET_CONTEXT_REG, ext->reg_count);
            b[n++] = ext->reg_index - PACKET3_SET_CONTEXT_REG_START;
            for (unsigned i = 0; i < ext->reg_count; ++i) b[n++] = ext->extent[i];
        }
    }
    b[n++] = PACKET3(PACKET3_SET_CONTEXT_REG, 1);
    b[n++] = GC(mmPA_SC_TILE_STEERING_OVERRIDE) - PACKET3_SET_CONTEXT_REG_START;
    b[n++] = 0;
    b[n++] = PACKET3(PACKET3_PREAMBLE_CNTL, 0);
    b[n++] = PACKET3_PREAMBLE_END_CLEAR_STATE;
    b[n++] = PACKET3(PACKET3_CLEAR_STATE, 0);
    b[n++] = 0;
    return n;
}

static bool stepRlc(Ctx& c, const Firmware& fw)
{
    // gfx_v10_0_hw_init, direct load: disable_gpa_mode, then gfx_v10_0_rlc_resume's last branch.
    CyanDirectLoad::disableGpaMode(kIo);
    uint32_t t = R(GC(mmRLC_CNTL));
    W(GC(mmRLC_CNTL), t & ~RLC_CNTL__RLC_ENABLE_F32_MASK);
    W(GC(mmRLC_CGCG_CGLS_CTRL), 0);
    W(GC(mmRLC_PG_CNTL), 0);
    CyanDirectLoad::Report rep = CyanDirectLoad::loadRlc(kIo, fw.rlc);
    if (rep.status != CyanDirectLoad::Status::Ok || gFail) {
        printf("STOP step 3: RLC load: %s\n", CyanDirectLoad::statusName(rep.status));
        return false;
    }
    uint32_t* csb = (uint32_t*)(c.area + kCsbOff);
    uint32_t dws = buildCsb(csb);
    be::flushWrites();
    uint64_t csbMc = c.areaMc() + kCsbOff;
    W(GC(mmRLC_CSIB_ADDR_HI), (uint32_t)(csbMc >> 32));
    W(GC(mmRLC_CSIB_ADDR_LO), (uint32_t)csbMc & 0xfffffffc);
    W(GC(mmRLC_CSIB_LENGTH), dws);
    // gfx_v10_0_update_spm_vmid_internal(adev, 0xf)
    uint32_t pre = R(GC(mmRLC_SPM_MC_CNTL));
    uint32_t data = (pre & ~RLC_SPM_MC_CNTL__RLC_SPM_VMID_MASK) | ((0xFu << RLC_SPM_MC_CNTL__RLC_SPM_VMID__SHIFT) & RLC_SPM_MC_CNTL__RLC_SPM_VMID_MASK);
    if (pre != data) W(GC(mmRLC_SPM_MC_CNTL), data);
    // gfx_v10_0_rlc_start, with gfx_v10_0_rlc_smu_handshake_cntl(false): RLC_PG_CNTL[23] = 1, "RLC will not issue any
    // message to SMU". Linux's default leaves it 0; we never want the RLC to talk to the SMU.
    W(GC(mmRLC_PG_CNTL), R(GC(mmRLC_PG_CNTL)) | 0x800000);
    W(GC(mmRLC_CNTL), R(GC(mmRLC_CNTL)) | RLC_CNTL__RLC_ENABLE_F32_MASK);
    be::delayUs(50);
    printf("step 3: RLC loaded (%u dwords), clear-state buffer %u dwords at GPU 0x%llx, RLC_CNTL 0x%08x RLC_PG_CNTL 0x%08x\n", rep.dataWrites, dws,
        (unsigned long long)csbMc, R(GC(mmRLC_CNTL)), R(GC(mmRLC_PG_CNTL)));
    return !gFail;
}

// ---- step 4: CP and SDMA microcode ----

static bool stepCp(Ctx& c, const Firmware& fw)
{
    CyanDirectLoad::Store store = { c.area + kStoreOff, c.areaMc() + kStoreOff, kStoreBytes };
    uint64_t need = CyanDirectLoad::storeBytesNeeded(fw.cp);
    CyanDirectLoad::Status st = CyanDirectLoad::stageCp(fw.cp, store);
    if (st != CyanDirectLoad::Status::Ok) {
        printf("STOP step 4: CP store (need 0x%llx bytes): %s\n", (unsigned long long)need, CyanDirectLoad::statusName(st));
        return false;
    }
    CyanDirectLoad::Report rep = CyanDirectLoad::loadCp(kIo, fw.cp, store);
    be::flushWrites();
    printf("step 4: CP load: %s (step %u), CP_STAT after halt 0x%x (%u us), icache waits %u %u %u %u us, %u jump-table dwords\n",
        CyanDirectLoad::statusName(rep.status), rep.step, rep.cpStatAfterHalt, rep.haltWaitUs, rep.icacheWaitUs[0], rep.icacheWaitUs[1],
        rep.icacheWaitUs[2], rep.icacheWaitUs[3], rep.dataWrites);
    if (rep.status != CyanDirectLoad::Status::Ok || gFail) return false;
    if (!CyanDirectLoad::storeIntact(fw.cp, store)) {
        printf("STOP step 4: the CP store does not read back\n");
        return false;
    }
    CyanDirectLoad::Report sd = CyanDirectLoad::loadSdma(kIo, fw.sdma0, fw.sdma1);
    printf("step 4: SDMA load: %s (%u dwords); CP code at GPU 0x%llx (0x%llx bytes)\n", CyanDirectLoad::statusName(sd.status), sd.dataWrites,
        (unsigned long long)store.gpu, (unsigned long long)need);
    return sd.status == CyanDirectLoad::Status::Ok && !gFail;
}

// ---- step 5: graphics ring ----

static void ringWrite(Ctx& c, uint32_t v)
{
    c.ring[c.wptr & (c.ringDw - 1)] = v;
    ++c.wptr;
}

// Two rules learned on the console (2026-11-19), both of which Linux keeps without saying so, because its submissions
// are 256-dword blocks in a ring of 8 such blocks: a packet must not straddle the end of the ring (the CP hung on a
// DMA_DATA at offset 0x7FB of 0x800), and the ring must not be handed more than it holds.
static bool ringDrain(Ctx& c, uint32_t timeoutUs, const char* what);
static void ringCommit(Ctx& c);

static void ringBegin(Ctx& c, uint32_t dwords)
{
    uint32_t off = (uint32_t)(c.wptr & (c.ringDw - 1));
    if (off + dwords > c.ringDw)
        while (c.wptr & (c.ringDw - 1)) ringWrite(c, PACKET3(PACKET3_NOP, 0x3FFF));
}

// amdgpu_ring_commit: pad to 256 dwords with one-dword NOPs, then move the write pointer.
static void ringCommit(Ctx& c)
{
    while (c.wptr & 0xFF) ringWrite(c, PACKET3(PACKET3_NOP, 0x3FFF));
    // gfx_v10_0_ring_set_wptr_gfx with a doorbell: the 64-bit, never-wrapping pointer goes to the poll address and
    // to the doorbell.
    *(volatile uint64_t*)(c.area + kWptrOff) = c.wptr;
    ((Shared*)(c.area + kSharedOff))->wptr = c.wptr;
    be::flushWrites();
    if (!be::doorbell64(kDoorbellIndex * 4, c.wptr)) {
        printf("  doorbell write failed\n");
        gFail = true;
    }
}

// Waits until the CP has read everything we wrote. The read pointer register wraps at the ring size.
static bool ringDrain(Ctx& c, uint32_t timeoutUs, const char* what)
{
    uint32_t last = 0;
    int64_t us = waitReg(GC(mmCP_RB0_RPTR), 0xFFFFFFFF, (uint32_t)(c.wptr & (c.ringDw - 1)), timeoutUs, &last);
    if (us < 0) {
        printf("  %s: the CP did not consume the ring: rptr 0x%x, wptr 0x%x, CP_STAT 0x%08x, GRBM_STATUS 0x%08x, fault status 0x%08x\n", what ? what : "ring", last,
            (uint32_t)(c.wptr & (c.ringDw - 1)), R(GC(mmCP_STAT)), R(GC(mmGRBM_STATUS)), R(GC(mmGCVM_L2_PROTECTION_FAULT_STATUS)));
        return false;
    }
    if (what) printf("  %s: ring consumed in %lld us (rptr 0x%x)\n", what, (long long)us, last);
    return true;
}

static bool stepRing(Ctx& c)
{
    // gfx_v10_0_cp_gfx_resume, ring 0 only.
    c.ring = (uint32_t*)(c.area + kRingOff);
    c.wptr = 0;
    Shared* shared = (Shared*)(c.area + kSharedOff);
    shared->magic = 0;                                   // set again once the ring test has passed
    shared->version = 1;
    shared->wptr = 0;
    shared->seq = 0;
    uint64_t ringMc = c.areaMc() + kRingOff, rptrMc = c.areaMc() + kRptrOff, wptrMc = c.areaMc() + kWptrOff;
    W(GC(mmCP_RB_WPTR_DELAY), 0);
    W(GC(mmCP_RB_VMID), 0);
    W(GC(mmGRBM_GFX_CNTL), R(GC(mmGRBM_GFX_CNTL)) & ~GRBM_GFX_CNTL__PIPEID_MASK);
    uint32_t bufsz = 0;
    while ((1u << bufsz) < kRingBytes / 8) ++bufsz;
    // Linux's (async) path starts from the register default, which has MIN_AVAILSZ = 2 and MIN_IB_AVAILSZ = 2
    // (Linux's value on the console: 0x00A0080A). Its legacy path writes 0 there, and with 0 the fetcher stalled at
    // the end of every burst of submissions (2026-11-19: 47-48 submissions left unread, RING_FETCHING_DATA set).
    uint32_t cntl = (bufsz << CP_RB0_CNTL__RB_BUFSZ__SHIFT) | ((bufsz - 2) << CP_RB0_CNTL__RB_BLKSZ__SHIFT) |
                    (2u << CP_RB0_CNTL__MIN_AVAILSZ__SHIFT) | (2u << CP_RB0_CNTL__MIN_IB_AVAILSZ__SHIFT);
    W(GC(mmCP_RB0_CNTL), cntl);
    W(GC(mmCP_RB0_WPTR), 0);
    W(GC(mmCP_RB0_WPTR_HI), 0);
    W(GC(mmCP_RB0_RPTR_ADDR), (uint32_t)rptrMc);
    W(GC(mmCP_RB0_RPTR_ADDR_HI), (uint32_t)(rptrMc >> 32) & CP_RB_RPTR_ADDR_HI__RB_RPTR_ADDR_HI_MASK);
    W(GC(mmCP_RB_WPTR_POLL_ADDR_LO), (uint32_t)wptrMc);
    W(GC(mmCP_RB_WPTR_POLL_ADDR_HI), (uint32_t)(wptrMc >> 32));
    be::delayUs(1000);
    W(GC(mmCP_RB0_CNTL), cntl);
    uint64_t rbAddr = ringMc >> 8;
    W(GC(mmCP_RB0_BASE), (uint32_t)rbAddr);
    W(GC(mmCP_RB0_BASE_HI), (uint32_t)(rbAddr >> 32));
    W(GC(mmCP_RB_ACTIVE), 1);
    // Linux leaves time slicing on for this queue (CP_GFX_HQD_QUANTUM 0xA01, QUANTUM_EN). Switching queues is the
    // MEC's business and we keep the MEC halted: after about a second of continuous work the fetcher stalled for good
    // (2026-11-19: stuck after 2953 submissions, POST_WPTR_GFX_BUSY and RING_FETCHING_DATA set). One queue, no slicing.
    uint32_t quantum = R(GC(mmCP_GFX_HQD_QUANTUM));
    W(GC(mmCP_GFX_HQD_QUANTUM), 0);
    printf("step 5: queue time slice was 0x%x, now 0x%x; HQD active %u, queue manager control 0x%x, MQD control 0x%x\n", quantum, R(GC(mmCP_GFX_HQD_QUANTUM)),
        R(GC(mmCP_GFX_HQD_ACTIVE)), R(GC(mmCP_GFX_HQD_QUE_MGR_CONTROL)), R(GC(mmCP_GFX_MQD_CONTROL)));
    // gfx_v10_0_cp_gfx_set_doorbell. Linux leaves this queue active in its KIQ-mapped form, and in that state the
    // write pointer is taken from the doorbell only (seen on the console: a write to CP_RB0_WPTR reads back 0 and the
    // CP does not move). Same doorbell as Linux: index 0x8B * 2, i.e. byte 0x458 of BAR2.
    uint32_t db = R(GC(mmCP_RB_DOORBELL_CONTROL));
    db = (db & ~CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) | ((kDoorbellIndex << CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT) & CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK);
    db |= CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK;
    W(GC(mmCP_RB_DOORBELL_CONTROL), db);
    W(GC(mmCP_RB_DOORBELL_RANGE_LOWER), (kDoorbellIndex << CP_RB_DOORBELL_RANGE_LOWER__DOORBELL_RANGE_LOWER__SHIFT) & CP_RB_DOORBELL_RANGE_LOWER__DOORBELL_RANGE_LOWER_MASK);
    W(GC(mmCP_RB_DOORBELL_RANGE_UPPER), CP_RB_DOORBELL_RANGE_UPPER__DOORBELL_RANGE_UPPER_MASK);

    // gfx_v10_0_cp_gfx_start
    W(GC(mmCP_MAX_CONTEXT), 8 - 1);
    W(GC(mmCP_DEVICE_ID), 1);
    uint32_t t = R(GC(mmCP_ME_CNTL));
    W(GC(mmCP_ME_CNTL), t & ~(CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK | CP_ME_CNTL__CE_HALT_MASK));
    uint32_t stat = 0;
    int64_t us = waitReg(GC(mmCP_STAT), 0xFFFFFFFF, 0, 100000, &stat);
    printf("step 5: ring at GPU 0x%llx (%u dwords); CP unhalted, CP_ME_CNTL 0x%08x, CP_STAT 0x%08x (%s)\n", (unsigned long long)ringMc, c.ringDw,
        R(GC(mmCP_ME_CNTL)), stat, us >= 0 ? "idle" : "not idle after 100 ms; Linux only warns here");
    if (gFail) return false;
    static uint32_t csb[4096];
    uint32_t n = buildCsb(csb);
    // gfx_v10_0_cp_gfx_start writes the same stream as the clear-state buffer into the ring, then SET_BASE.
    if (n + 4 + 256 > c.ringDw) { printf("STOP step 5: the clear-state stream does not fit the ring\n"); return false; }
    for (uint32_t i = 0; i < n; ++i) ringWrite(c, csb[i]);
    ringWrite(c, PACKET3(PACKET3_SET_BASE, 2));
    ringWrite(c, PACKET3_BASE_INDEX(CE_PARTITION_BASE));
    ringWrite(c, 0x8000);
    ringWrite(c, 0x8000);
    ringCommit(c);
    return ringDrain(c, 500000, "step 5 (clear state)");
}

// ---- step 6: ring test ----

static bool stepRingTest(Ctx& c)
{
    uint32_t scratch = GC(mmSCRATCH_REG0);
    W(scratch, 0xCAFEDEAD);
    ringWrite(c, PACKET3(PACKET3_SET_UCONFIG_REG, 1));
    ringWrite(c, scratch - PACKET3_SET_UCONFIG_REG_START);
    ringWrite(c, 0xDEADBEEF);
    ringCommit(c);
    uint32_t last = 0;
    int64_t us = waitReg(scratch, 0xFFFFFFFF, 0xDEADBEEF, 200000, &last);
    if (us < 0) {
        printf("step 6: RING TEST FAILED: scratch register reads 0x%08x\n", last);
        ringDrain(c, 1000, "step 6");
        return false;
    }
    printf("step 6: RING TEST PASSED in %lld us: the CP executed our command\n", (long long)us);
    ((Shared*)(c.area + kSharedOff))->magic = kSharedMagic;   // from here on the library may attach
    *(volatile uint64_t*)(c.area + kFenceOff) = 0;
    // The library's own bookkeeping (lib/ps5drm.cpp, State at area + 0xA00000) describes the ring that was just
    // replaced: its first word (the magic) is cleared so that the next client starts it afresh.
    *(volatile uint32_t*)(c.area + 0xA00000) = 0;
    be::flushWrites();
    return true;
}

// ---- step 7: a rectangle drawn by the GPU ----

static bool fill(Ctx& c, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour)
{
    if (!c.surfMc) { printf("no scan-out address; not drawing\n"); return false; }
    if (w == 0 || h == 0 || x + w > c.pitchPx || y + h > 1080 || h > 600) { printf("rectangle out of range\n"); return false; }
    // The CPU's view of the same pixels, to read the result back.
    uint64_t rowPhys = c.carvePhys + (c.surfMc - c.mcBase) + ((uint64_t)y * c.pitchPx + x) * 4;
    uint64_t mapPhys = rowPhys & ~0xFFFFull, mapBytes = (((uint64_t)h * c.pitchPx * 4 + (rowPhys - mapPhys)) + 0xFFFF) & ~0xFFFFull;
    uint8_t* map = (uint8_t*)be::map(mapPhys, mapBytes, true);
    if (!map) return false;
    volatile uint32_t* px = (volatile uint32_t*)(map + (rowPhys - mapPhys));
    uint32_t before = px[0];
    // CP DMA, one packet per row: source = the 32-bit value in the packet, destination = memory (VMID 0 address).
    for (uint32_t row = 0; row < h; ++row) {
        uint64_t dst = c.surfMc + ((uint64_t)(y + row) * c.pitchPx + x) * 4;
        if (row && row % 32 == 0) {                       // 32 rows = 224 dwords: one block per commit
            ringCommit(c);
            if (!ringDrain(c, 500000, nullptr)) return false;
        }
        ringBegin(c, 7);
        ringWrite(c, PACKET3(PACKET3_DMA_DATA, 5));
        ringWrite(c, PACKET3_DMA_DATA_ENGINE(0) | PACKET3_DMA_DATA_DST_SEL(0) | PACKET3_DMA_DATA_SRC_SEL(2) | PACKET3_DMA_DATA_CP_SYNC);
        ringWrite(c, colour);
        ringWrite(c, 0);
        ringWrite(c, (uint32_t)dst);
        ringWrite(c, (uint32_t)(dst >> 32));
        ringWrite(c, (w * 4) & 0x03FFFFFF);
    }
    uint32_t scratch = GC(mmSCRATCH_REG0);
    W(scratch, 0);
    ringCommit(c);
    if (!ringDrain(c, 500000, nullptr)) return false;
    ringWrite(c, PACKET3(PACKET3_SET_UCONFIG_REG, 1));
    ringWrite(c, scratch - PACKET3_SET_UCONFIG_REG_START);
    ringWrite(c, 0x600DF111);
    ringCommit(c);
    uint32_t last = 0;
    int64_t us = waitReg(scratch, 0xFFFFFFFF, 0x600DF111, 500000, &last);
    uint32_t after = px[0], afterLast = px[(uint64_t)(h - 1) * c.pitchPx + (w - 1)];
    printf("step 7: fill %ux%u at %u,%u colour 0x%08x: %s; pixel before 0x%08x, after 0x%08x, last 0x%08x => %s\n", w, h, x, y, colour,
        us >= 0 ? "commands finished" : "commands did NOT finish", before, after, afterLast,
        (after == colour && afterLast == colour) ? "THE GPU DREW IT" : "not drawn");
    if (us < 0) ringDrain(c, 1000, "step 7");
    return us >= 0 && after == colour && afterLast == colour;
}

// ---- step 8: a user address space (VMID 1), an indirect buffer and a fence ----
//
// What a program's rendering needs from a kernel driver, done by hand once: page tables that map some of our VRAM at a
// GPU virtual address, a command buffer that lives there, its submission from the ring, and a fence that tells us it
// has finished. Formats from amdgpu_vm.h / gmc_v10_0.c: four levels of 512 entries (PDB2, PDB1, PDB0, PTB), 4 KiB
// pages; an entry is the physical address of the next table or of the page, plus flags. The context's base register
// holds the root's physical address with the valid bit, as Linux's GART base does (0x4_6FF00001).
static const uint64_t kUserVa = 0x100000000ull;                       // where it appears there
static const uint64_t kPteValid = 1, kPteExecutable = 1 << 4, kPteReadable = 1 << 5, kPteWriteable = 1 << 6;

static bool stepVm(Ctx& c)
{
    uint64_t* pt = (uint64_t*)(c.area + kPtOff);
    uint64_t ptPhys = c.areaPhys() + kPtOff, userPhys = c.areaPhys() + kUserOff;
    memset(pt, 0, 4 * 0x1000);
    pt[(kUserVa >> 39) & 511] = (ptPhys + 0x1000) | kPteValid;
    pt[512 + ((kUserVa >> 30) & 511)] = (ptPhys + 0x2000) | kPteValid;
    pt[1024 + ((kUserVa >> 21) & 511)] = (ptPhys + 0x3000) | kPteValid;
    for (uint64_t i = 0; i < kUserBytes / 0x1000; ++i)
        pt[1536 + i] = (userPhys + i * 0x1000) | kPteValid | kPteReadable | kPteWriteable | kPteExecutable;
    memset(c.area + kUserOff, 0, 0x20000);
    *(volatile uint32_t*)(c.area + kFenceOff) = 0;
    be::flushWrites();
    printf("step 8: VMID 1 as Linux left it: CNTL 0x%08x, range 0x%08x..0x%x:%08x (<<12)\n", R(GC(mmGCVM_CONTEXT1_CNTL)),
        R(GC(mmGCVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32)), R(GC(mmGCVM_CONTEXT1_PAGE_TABLE_END_ADDR_HI32)), R(GC(mmGCVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32)));
    W(GC(mmGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32), (uint32_t)(ptPhys | kPteValid));
    W(GC(mmGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_HI32), (uint32_t)(ptPhys >> 32));
    if (!flushTlb(1)) { printf("STOP step 8: VMID 1 TLB flush not acknowledged\n"); return false; }

    // The command buffer, at virtual address kUserVa: fill 1 KiB at kUserVa + 0x10000, then mark the scratch register.
    uint32_t* ib = (uint32_t*)(c.area + kUserOff);
    uint32_t n = 0, scratch = GC(mmSCRATCH_REG0);
    const uint64_t target = kUserVa + 0x10000;
    const uint32_t pattern = 0x1B0CAFE5, mark = 0x01B0CAFE, seq = 0x5EC0001;
    ib[n++] = PACKET3(PACKET3_DMA_DATA, 5);
    ib[n++] = PACKET3_DMA_DATA_ENGINE(0) | PACKET3_DMA_DATA_DST_SEL(0) | PACKET3_DMA_DATA_SRC_SEL(2) | PACKET3_DMA_DATA_CP_SYNC;
    ib[n++] = pattern;
    ib[n++] = 0;
    ib[n++] = (uint32_t)target;
    ib[n++] = (uint32_t)(target >> 32);
    ib[n++] = 0x400;
    ib[n++] = PACKET3(PACKET3_SET_UCONFIG_REG, 1);
    ib[n++] = scratch - PACKET3_SET_UCONFIG_REG_START;
    ib[n++] = mark;
    while (n & 7) ib[n++] = PACKET3(PACKET3_NOP, 0x3FFF);
    be::flushWrites();
    W(scratch, 0);
    // amdgpu_ib_schedule, the parts that matter without preemption: context control (gfx_v10_0_ring_emit_cntxcntl with
    // a context switch), the buffer (gfx_v10_0_ring_emit_ib_gfx), the fence (gfx_v10_0_ring_emit_fence, 32-bit, no
    // interrupt) at a VMID 0 address in our area.
    uint64_t fenceMc = c.areaMc() + kFenceOff;
    ringWrite(c, PACKET3(PACKET3_CONTEXT_CONTROL, 1));
    ringWrite(c, 0x80000000 | 0x8001 | 0x01000000 | 0x10002);
    ringWrite(c, 0);
    ringWrite(c, PACKET3(PACKET3_INDIRECT_BUFFER, 2));
    ringWrite(c, (uint32_t)kUserVa);
    ringWrite(c, (uint32_t)(kUserVa >> 32));
    ringWrite(c, n | (1u << 24));
    ringWrite(c, PACKET3(PACKET3_RELEASE_MEM, 6));
    ringWrite(c, PACKET3_RELEASE_MEM_GCR_SEQ | PACKET3_RELEASE_MEM_GCR_GL2_WB | PACKET3_RELEASE_MEM_GCR_GLM_INV | PACKET3_RELEASE_MEM_GCR_GLM_WB |
                 PACKET3_RELEASE_MEM_CACHE_POLICY(3) | PACKET3_RELEASE_MEM_EVENT_TYPE(0x14 /* CACHE_FLUSH_AND_INV_TS_EVENT */) | PACKET3_RELEASE_MEM_EVENT_INDEX(5));
    ringWrite(c, PACKET3_RELEASE_MEM_DATA_SEL(1) | PACKET3_RELEASE_MEM_INT_SEL(0));
    ringWrite(c, (uint32_t)fenceMc);
    ringWrite(c, (uint32_t)(fenceMc >> 32));
    ringWrite(c, seq);
    ringWrite(c, 0);
    ringWrite(c, 0);
    ringCommit(c);
    volatile uint32_t* fence = (volatile uint32_t*)(c.area + kFenceOff);
    uint64_t start = be::nowUs();
    while (*fence != seq && be::nowUs() - start < 500000) be::delayUs(20);
    uint64_t took = be::nowUs() - start;
    volatile uint32_t* out = (volatile uint32_t*)(c.area + kUserOff + 0x10000);
    bool fenceOk = *fence == seq, markOk = R(scratch) == mark, fillOk = out[0] == pattern && out[255] == pattern && out[256] == 0;
    printf("step 8: fence %s (0x%08x after %llu us), scratch 0x%08x (%s), buffer %08x .. %08x | %08x (%s), fault status 0x%08x\n",
        fenceOk ? "signalled" : "NOT signalled", (uint32_t)*fence, (unsigned long long)took, R(scratch), markOk ? "ok" : "wrong", (uint32_t)out[0],
        (uint32_t)out[255], (uint32_t)out[256], fillOk ? "filled through the user mapping" : "wrong", R(GC(mmGCVM_L2_PROTECTION_FAULT_STATUS)));
    if (!fenceOk) ringDrain(c, 1000, "step 8");
    if (fenceOk && markOk && fillOk) printf("step 8: A COMMAND BUFFER RAN IN ITS OWN ADDRESS SPACE AND SIGNALLED ITS FENCE\n");
    return fenceOk && markOk && fillOk;
}

int libtest(unsigned rounds);
int libtest2(unsigned rounds);

// Diagnostics for a stalled ring: pointers, the ring's contents around the read pointer, and optionally the doorbell
// written once more with the current write pointer ("kick"). Changes nothing else.
static int ringdump(Ctx& c, bool kick)
{
    if (!discover(c, false)) return 1;
    c.area = (uint8_t*)be::map(c.areaPhys(), kAreaBytes, true);
    if (!c.area) return 1;
    Shared* sh = (Shared*)(c.area + kSharedOff);
    uint32_t* ring = (uint32_t*)(c.area + kRingOff);
    uint32_t mask = (uint32_t)(kRingBytes / 4) - 1, rptr = R(GC(mmCP_RB0_RPTR));
    printf("shared: magic 0x%08x wptr 0x%llx (ring offset 0x%x) seq %llu; fence %llu; poll memory 0x%llx; rptr register 0x%x; rptr report memory 0x%x\n", sh->magic,
        (unsigned long long)sh->wptr, (uint32_t)(sh->wptr & mask), (unsigned long long)sh->seq, (unsigned long long)*(volatile uint64_t*)(c.area + kFenceOff),
        (unsigned long long)*(volatile uint64_t*)(c.area + kWptrOff), rptr, *(volatile uint32_t*)(c.area + kRptrOff));
    printf("HQD wptr 0x%x:%08x, CP_STAT 0x%08x, GRBM_STATUS 0x%08x, CPF_STATUS 0x%08x, CPF_STALLED 0x%x, doorbell control 0x%08x\n", R(GC(mmCP_GFX_HQD_WPTR_HI)),
        R(GC(mmCP_GFX_HQD_WPTR)), R(GC(mmCP_STAT)), R(GC(mmGRBM_STATUS)), R(GC(mmCP_CPF_STATUS)), R(GC(mmCP_CPF_STALLED_STAT1)), R(GC(mmCP_RB_DOORBELL_CONTROL)));
    uint32_t from = (rptr & ~0xFFu) - 0x100;
    for (uint32_t blk = 0; blk < 4; ++blk) {
        uint32_t base = (from + blk * 0x100) & mask;
        printf("ring 0x%04x:", base);
        uint32_t nops = 0;
        for (uint32_t i = 0; i < 0x100; ++i) {
            uint32_t v = ring[(base + i) & mask];
            if (v == PACKET3(PACKET3_NOP, 0x3FFF)) { ++nops; continue; }
            if (nops) { printf(" [%u nop]", nops); nops = 0; }
            printf(" %08x", v);
        }
        if (nops) printf(" [%u nop]", nops);
        printf("\n");
    }
    if (kick) {
        if (!be::arm(true)) return 1;
        // A block of NOPs and a doorbell with a new value: tells a hung fetcher from a doorbell that was not taken.
        for (uint32_t i = 0; i < 0x100; ++i) ring[(sh->wptr + i) & mask] = PACKET3(PACKET3_NOP, 0x3FFF);
        sh->wptr += 0x100;
        *(volatile uint64_t*)(c.area + kWptrOff) = sh->wptr;
        be::flushWrites();
        be::doorbell64(kDoorbellIndex * 4, sh->wptr);
        be::delayUs(200000);
        printf("after a NOP block and a new doorbell value: rptr 0x%x, fence %llu, CP_STAT 0x%08x\n", R(GC(mmCP_RB0_RPTR)), (unsigned long long)*(volatile uint64_t*)(c.area + kFenceOff),
            R(GC(mmCP_STAT)));
        be::arm(false);
    }
    return 0;
}

// ---- display: can we point scan-out at our own image? (the first display register write of this driver) ----
// What Linux's hubp2_program_surface_flip_and_addr does for an address-only flip: write the high half, then the low
// half; the low write arms the double-buffered pair and the display takes it at the next frame. Nothing else is
// written: no timing, no link, no format. The mode the firmware/Linux set stays.
// At 2560x1440 Linux splits the picture over two display pipes (left and right half, same image address), so a flip
// writes every active pipe, inside the timing generator's update lock so that all take it on the same frame
// (optc1_lock / optc1_unlock). "Earliest in use" is what Linux's hubp2_is_flip_pending compares against.
struct Pipe { uint32_t addrLo, addrHi, inUseLo, inUseHi, vpDim, vpStart, cntl; };
#define HUBPREQ(n, r) DMU(mmHUBPREQ##n##_##r)
#define HUBP(n, r) DMU(mmHUBP##n##_##r)
#define PIPE(n) { HUBPREQ(n, DCSURF_PRIMARY_SURFACE_ADDRESS), HUBPREQ(n, DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH), HUBPREQ(n, DCSURF_SURFACE_EARLIEST_INUSE), \
                  HUBPREQ(n, DCSURF_SURFACE_EARLIEST_INUSE_HIGH), HUBP(n, DCSURF_PRI_VIEWPORT_DIMENSION), HUBP(n, DCSURF_PRI_VIEWPORT_START), HUBP(n, DCHUBP_CNTL) }
static const Pipe kPipes[4] = { PIPE(0), PIPE(1), PIPE(2), PIPE(3) };
static uint32_t gPipeMask;                                  // the pipes that show the picture

static uint64_t pipeAddr(const Pipe& p) { return ((uint64_t)(R(p.addrHi) & 0xFFFF) << 32) | R(p.addrLo); }
static uint64_t pipeInUse(const Pipe& p) { return ((uint64_t)(R(p.inUseHi) & 0xFFFF) << 32) | R(p.inUseLo); }

static uint64_t surfaceInUse()                               // ~0 while the pipes disagree
{
    uint64_t v = 0;
    bool first = true;
    for (uint32_t n = 0; n < 4; ++n) {
        if (!(gPipeMask & (1u << n))) continue;
        uint64_t a = pipeInUse(kPipes[n]);
        if (first) { v = a; first = false; }
        else if (a != v) return ~0ull;
    }
    return v;
}

static uint32_t gLockMisses;
static const uint32_t kUpdateLockStatus = 0x100;            // OTG_MASTER_UPDATE_LOCK.UPDATE_LOCK_STATUS (dcn_2_0_1_sh_mask.h)
static const uint32_t kHubpBlankEn = 0x1;                   // DCHUBP_CNTL.HUBP_BLANK_EN

static int64_t flipTo(uint64_t phys, uint32_t timeoutUs)
{
    uint64_t t0 = be::nowUs();
    W(DMU(mmOTG0_OTG_MASTER_UPDATE_LOCK), 1);
    if (waitReg(DMU(mmOTG0_OTG_MASTER_UPDATE_LOCK), kUpdateLockStatus, kUpdateLockStatus, 10000) < 0)
        ++gLockMisses;
    for (uint32_t n = 0; n < 4; ++n) {
        if (!(gPipeMask & (1u << n))) continue;
        W(kPipes[n].addrHi, (uint32_t)(phys >> 32));
        W(kPipes[n].addrLo, (uint32_t)phys);
    }
    W(DMU(mmOTG0_OTG_MASTER_UPDATE_LOCK), 0);
    for (;;) {
        if (surfaceInUse() == phys) return (int64_t)(be::nowUs() - t0);
        if (be::nowUs() - t0 > timeoutUs) return -1;
    }
}

static int flipTest(Ctx& c, uint32_t seconds, uint32_t cycles, const char* backTo)
{
    if (!discover(c, false)) return 1;
    uint64_t oldPhys = ((uint64_t)(R(DMU(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH)) & 0xFFFF) << 32) | R(DMU(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS));
    uint32_t w = 0, h = 0;
    gPipeMask = 0;
    for (uint32_t n = 0; n < 4; ++n) {
        const Pipe& p = kPipes[n];
        uint32_t dim = R(p.vpDim), start = R(p.vpStart), cntl = R(p.cntl);
        uint64_t addr = pipeAddr(p), inUse = pipeInUse(p);
        bool on = !(cntl & kHubpBlankEn) && addr == oldPhys && inUse == oldPhys && (dim & 0x3FFF) && ((dim >> 16) & 0x3FFF);
        printf("pipe %u: address 0x%llx earliest in use 0x%llx viewport %ux%u at %u,%u HUBP_CNTL 0x%08x%s\n", n, (unsigned long long)addr, (unsigned long long)inUse,
            dim & 0x3FFF, (dim >> 16) & 0x3FFF, start & 0x3FFF, (start >> 16) & 0x3FFF, cntl, on ? "  <- shows the picture" : "");
        if (!on) continue;
        gPipeMask |= 1u << n;
        if ((start & 0x3FFF) + (dim & 0x3FFF) > w) w = (start & 0x3FFF) + (dim & 0x3FFF);
        if (((dim >> 16) & 0x3FFF) > h) h = (dim >> 16) & 0x3FFF;
    }
    printf("picture: %ux%u, pitch %u, pipes 0x%x\n", w, h, c.pitchPx, gPipeMask);
    printf("plane: SURFACE_CONFIG 0x%08x ADDR_CONFIG 0x%08x TILING_CONFIG 0x%08x SURFACE_CONTROL 0x%08x FLIP_CONTROL 0x%08x FLIP_CONTROL2 0x%08x FLIP_INTERRUPT 0x%08x HUBP_CNTL 0x%08x\n",
        R(DMU(mmHUBP0_DCSURF_SURFACE_CONFIG)), R(DMU(mmHUBP0_DCSURF_ADDR_CONFIG)), R(DMU(mmHUBP0_DCSURF_TILING_CONFIG)), R(DMU(mmHUBPREQ0_DCSURF_SURFACE_CONTROL)),
        R(DMU(mmHUBPREQ0_DCSURF_FLIP_CONTROL)), R(DMU(mmHUBPREQ0_DCSURF_FLIP_CONTROL2)), R(DMU(mmHUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT)), R(DMU(mmHUBP0_DCHUBP_CNTL)));
    printf("timing: OTG_CONTROL 0x%08x STATUS 0x%08x position 0x%08x frame 0x%x MASTER_UPDATE_LOCK 0x%08x GLOBAL_CONTROL0 0x%08x INTERRUPT_CONTROL 0x%08x VERTICAL_INTERRUPT0/1/2 0x%08x 0x%08x 0x%08x\n",
        R(DMU(mmOTG0_OTG_CONTROL)), R(DMU(mmOTG0_OTG_STATUS)), R(DMU(mmOTG0_OTG_STATUS_POSITION)), R(DMU(mmOTG0_OTG_STATUS_FRAME_COUNT)), R(DMU(mmOTG0_OTG_MASTER_UPDATE_LOCK)),
        R(DMU(mmOTG0_OTG_GLOBAL_CONTROL0)), R(DMU(mmOTG0_OTG_INTERRUPT_CONTROL)), R(DMU(mmOTG0_OTG_VERTICAL_INTERRUPT0_CONTROL)), R(DMU(mmOTG0_OTG_VERTICAL_INTERRUPT1_CONTROL)),
        R(DMU(mmOTG0_OTG_VERTICAL_INTERRUPT2_CONTROL)));
    if (gFail) return 1;
    if (backTo) {                                            // repair: put a known address back
        uint64_t phys = strtoull(backTo, NULL, 16);
        if (phys < c.carvePhys || phys >= c.carvePhys + c.vramBytes) { printf("STOP: not an address in the carve-out\n"); return 1; }
        if (!be::arm(true)) return 1;
        int64_t us = flipTo(phys, 500000);
        be::arm(false);
        printf("flip to 0x%llx: %s\n", (unsigned long long)phys, us < 0 ? "NOT TAKEN" : "taken");
        return us < 0;
    }
    uint64_t bytes = (uint64_t)c.pitchPx * 4 * h, newPhys = c.carvePhys + kHeapStart;
    if (oldPhys < c.carvePhys || oldPhys >= c.carvePhys + c.vramBytes || w < 640 || w > c.pitchPx || h < 480 || h > 2160 || !gPipeMask || surfaceInUse() != oldPhys) {
        printf("STOP: the plane is not in the state this test expects (physical address in the carve-out, in use)\n");
        return 1;
    }
    if (oldPhys + bytes > newPhys && oldPhys < newPhys + bytes) { printf("STOP: the current image is where the test image would go\n"); return 1; }
    if (p5_clients() > 0) { printf("STOP: another program is using the GPU (its buffers live where the test image goes); close it first\n"); return 1; }
    uint32_t* img = (uint32_t*)be::map(newPhys, (bytes + 0xFFFF) & ~0xFFFFull, true);
    if (!img) { printf("STOP: cannot map the test image\n"); return 1; }
    static const uint32_t bars[8] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x202020 };
    for (uint32_t y = 0; y < h; ++y) {
        uint32_t* row = img + (uint64_t)y * c.pitchPx;
        for (uint32_t x = 0; x < c.pitchPx; ++x) {
            uint32_t v = bars[(x * 8 / w) & 7];
            if (x < 16 || y < 16 || x + 16 >= w || y + 16 >= h) v = 0xFF8000;            // orange border: the edges of the picture
            else if (y > h / 2 - 60 && y < h / 2 + 60) v = ((x * 255 / w) * 0x010101);      // grey ramp across the middle
            row[x] = v;
        }
    }
    be::flushWrites();
    if (!be::arm(true)) return 1;
    uint32_t f0 = R(DMU(mmOTG0_OTG_STATUS_FRAME_COUNT));
    int64_t us = flipTo(newPhys, 500000);
    if (us < 0) {
        printf("flip NOT TAKEN within 500 ms: in use 0x%llx, FLIP_CONTROL 0x%08x; writing the old address back\n", (unsigned long long)surfaceInUse(), R(DMU(mmHUBPREQ0_DCSURF_FLIP_CONTROL)));
        flipTo(oldPhys, 500000);
        be::arm(false);
        return 1;
    }
    printf("FLIP TAKEN: scan-out now reads our image at 0x%llx, %lld us after the write (frame 0x%x -> 0x%x)\n", (unsigned long long)newPhys, (long long)us, f0, R(DMU(mmOTG0_OTG_STATUS_FRAME_COUNT)));
    uint64_t t0 = be::nowUs();
    f0 = R(DMU(mmOTG0_OTG_STATUS_FRAME_COUNT));
    be::delayUs(seconds * 1000000);
    uint32_t f1 = R(DMU(mmOTG0_OTG_STATUS_FRAME_COUNT));
    double held = (double)(be::nowUs() - t0) / 1e6;
    printf("held %.2f s: %u frames = %.2f Hz\n", held, f1 - f0, (f1 - f0) / held);
    int64_t lo = 1 << 30, hi = 0, sum = 0;
    uint32_t done = 0;
    uint64_t t1 = be::nowUs();
    for (uint32_t i = 0; i < cycles; ++i) {                 // alternate the two images, one flip after the other
        us = flipTo(i & 1 ? newPhys : oldPhys, 500000);
        if (us < 0) { printf("flip %u NOT TAKEN\n", i); break; }
        lo = us < lo ? us : lo; hi = us > hi ? us : hi; sum += us; ++done;
    }
    double span = (double)(be::nowUs() - t1) / 1e6;
    if (done) printf("%u back-to-back flips in %.2f s = %.1f per second; write-to-taken %lld / %lld / %lld us (min / average / max)\n", done, span, done / span,
        (long long)lo, (long long)(sum / done), (long long)hi);
    us = flipTo(oldPhys, 500000);
    printf("update lock not confirmed %u times\n", gLockMisses);
    printf("%s (in use 0x%llx); FLIP_CONTROL 0x%08x FLIP_INTERRUPT 0x%08x\n", us < 0 ? "RESTORE NOT TAKEN" : "old image restored", (unsigned long long)surfaceInUse(),
        R(DMU(mmHUBPREQ0_DCSURF_FLIP_CONTROL)), R(DMU(mmHUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT)));
    be::arm(false);
    return us < 0 || gFail;
}

int main(int argc, char** argv)
{
    const char* cmd = argc > 1 ? argv[1] : "";
    Ctx c;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(cmd, "libtest")) return libtest(argc > 2 ? strtoul(argv[2], NULL, 0) : 2000);
    if (!strcmp(cmd, "scanshot")) {                         // scanshot <file.bmp>: the picture the display is really showing
        int r = argc > 2 ? p5_scanout_snapshot(argv[2]) : -22;
        printf(r ? "scanshot failed: %d\n" : "scanshot written\n", r);
        return r ? 1 : 0;
    }
    if (!strcmp(cmd, "housekeeping")) {                     // clean up after programs that died (the watchdog runs this)
        int r = p5_housekeeping();
        if (r > 0) printf("housekeeping:%s%s\n", (r & 1) ? " screen given back to the desktop" : "", (r & 2) ? " dead clients removed" : "");
        return r < 0 ? 1 : 0;
    }
    if (!strcmp(cmd, "halt-for-test")) {                    // stops the command processor on purpose: for testing the watchdog only
        if (!be::open() || !be::arm(true)) return 2;
        uint32_t v = 0;
        if (!be::rd(GC(mmCP_ME_CNTL), v)) return 2;
        be::wr(GC(mmCP_ME_CNTL), v | CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK | CP_ME_CNTL__CE_HALT_MASK);
        be::flushWrites();
        be::arm(false);
        printf("command processor halted on purpose (was 0x%08x)\n", v);
        return 0;
    }
    if (!strcmp(cmd, "mapprobe")) {                         // mapprobe [heap 0/1] [ms]: map like a new client, without attaching
        int heap = argc > 2 ? atoi(argv[2]) : 1, ms = argc > 3 ? atoi(argv[3]) : 100;
        int us = p5_map_probe(heap, ms);
        printf("mapping took %d us\n", us);
        return us < 0;
    }
    if (!strcmp(cmd, "state")) {                            // the library's shared state: clients, pids, memory
        static char text[8192];
        int r = p5_state_text(text, sizeof text);
        printf("%s", r < 0 ? "cannot read the state\n" : text);
        return r < 0 ? 1 : 0;
    }
    if (!strcmp(cmd, "clients")) {                          // process ids of the programs that have the GPU open, one per line
        uint32_t pids[16];
        int n = p5_client_pids(pids, 16);
        for (int i = 0; i < n; ++i) printf("%u\n", pids[i]);
        return n < 0 ? 9 : 0;
    }
    if (!strcmp(cmd, "health")) {                           // for the watchdog: exit code 0 fine, 1 not started, 2 halted, 3 work waiting
        uint64_t seq = 0, fence = 0;
        int r = p5_health(&seq, &fence);
        printf("health %d fence %llu seq %llu (%s)\n", r, (unsigned long long)fence, (unsigned long long)seq,
               r == 0 ? "fine" : r == 1 ? "GPU not started" : r == 2 ? "COMMAND PROCESSOR HALTED" : r == 3 ? "work waiting" : "cannot look");
        return r < 0 ? 9 : r;
    }
    if (!strcmp(cmd, "libtest2")) return libtest2(argc > 2 ? strtoul(argv[2], NULL, 0) : 2000);
    if (!strcmp(cmd, "vramscan")) {                         // vramscan [first MiB] [MiB count]: read-only look at GPU memory
        // One line per 16 MiB: for each MiB a character ('.' all zero, 'f' all ones, digit = tenths of non-zero 64-bit
        // words) and a checksum per line. Run twice to see what changes. Reads only.
        if (!be::open()) return 2;
        uint32_t fbOffset = 0;
        if (!be::rd(GC(mmGCMC_VM_FB_OFFSET), fbOffset)) return 2;
        uint64_t carve = (uint64_t)fbOffset << 24, first = argc > 2 ? strtoull(argv[2], NULL, 0) : 0, count = argc > 3 ? strtoull(argv[3], NULL, 0) : 256;
        for (uint64_t m = first; m < first + count; m += 16) {
            const uint64_t* p = (const uint64_t*)be::map(carve + (m << 20), 16ull << 20, true);
            if (!p) { printf("%4llu MiB: cannot map\n", (unsigned long long)m); continue; }
            char line[17] = {0};
            uint64_t sum = 0;
            for (unsigned k = 0; k < 16; ++k) {
                uint64_t nz = 0, ones = 0;
                for (unsigned i = 0; i < (1u << 20) / 8; ++i) {
                    uint64_t v = p[(uint64_t)k * ((1u << 20) / 8) + i];
                    nz += v != 0;
                    ones += v == ~0ull;
                    sum = (sum ^ v) * 0x100000001B3ull;
                }
                line[k] = nz == 0 ? '.' : ones == (1u << 20) / 8 ? 'f' : (char)('0' + (nz * 9) / ((1u << 20) / 8));
            }
            printf("%4llu MiB: %s  %016llx\n", (unsigned long long)m, line, (unsigned long long)sum);
            be::unmap((void*)p);
        }
        return 0;
    }
    if (!strcmp(cmd, "ringdump") || !strcmp(cmd, "kick")) return be::open() ? ringdump(c, !strcmp(cmd, "kick")) : 2;
    if (!strcmp(cmd, "flip")) {                             // flip [seconds] [back-to-back flips]  |  flip back <physical address, hex>
        if (!be::open()) return 2;
        if (argc > 3 && !strcmp(argv[2], "back")) return flipTest(c, 0, 0, argv[3]);
        return flipTest(c, argc > 2 ? strtoul(argv[2], NULL, 0) : 4, argc > 3 ? strtoul(argv[3], NULL, 0) : 120, nullptr);
    }
    if (!strcmp(cmd, "shmem")) {
        // Per-VMID shader memory configuration (gfx_v10_0_constants_init programs it; banked by GRBM_GFX_CNTL.VMID).
        if (!be::open() || !be::arm(true)) return 2;
        uint32_t sel = R(GC(mmGRBM_GFX_CNTL));
        for (uint32_t vmid = 0; vmid < 16; ++vmid) {
            W(GC(mmGRBM_GFX_CNTL), (sel & ~GRBM_GFX_CNTL__VMID_MASK) | (vmid << GRBM_GFX_CNTL__VMID__SHIFT));
            printf("vmid %2u: SH_MEM_CONFIG 0x%08x SH_MEM_BASES 0x%08x\n", vmid, R(GC(mmSH_MEM_CONFIG)), R(GC(mmSH_MEM_BASES)));
        }
        W(GC(mmGRBM_GFX_CNTL), sel);
        printf("GRBM_CNTL 0x%08x DB_RING_CONTROL 0x%08x GDS_VMID1 base/size 0x%x/0x%x UTCL1_CTRL 0x%08x GB_ADDR_CONFIG 0x%08x\n", R(GC(mmGRBM_CNTL)), R(GC(mmDB_RING_CONTROL)),
            R(GC(mmGDS_VMID0_BASE) + 2), R(GC(mmGDS_VMID0_SIZE) + 2), R(GC(mmUTCL1_CTRL)), R(GC(mmGB_ADDR_CONFIG)));
        printf("golden: CB_HW_CONTROL_3 0x%08x CB_HW_CONTROL_4 0x%08x DB_DEBUG4 0x%08x GCR_GENERAL_CNTL 0x%08x GE_FAST_CLKS 0x%08x PA_SC_ENHANCE 0x%08x SQ_LDS_CLK_CTRL 0x%08x CP_SD_CNTL 0x%08x\n",
            R(GC(mmCB_HW_CONTROL_3)), R(GC(mmCB_HW_CONTROL_4)), R(GC(mmDB_DEBUG4)), R(GC(mmGCR_GENERAL_CNTL)), R(GC(mmGE_FAST_CLKS)), R(GC(mmPA_SC_ENHANCE)), R(GC(mmSQ_LDS_CLK_CTRL)), R(GC(mmCP_SD_CNTL)));
        be::arm(false);
        return gFail ? 1 : 0;
    }
    if (strcmp(cmd, "look") && strcmp(cmd, "run")) {
        printf("usage: ps5gpu2 look | run <firmware dir> [upto 1-8] [x y w h rgb] | libtest [submissions]\n");
        return 2;
    }
    if (!be::open()) return 2;
    if (!strcmp(cmd, "look")) {
        look(c);
        return gFail ? 1 : 0;
    }
    if (argc < 3) { printf("run needs the firmware directory\n"); return 2; }
    if (p5_clients() > 0) { printf("STOP: %d program(s) are using the GPU; restarting it would pull the ring from under them. Close them first.\n", p5_clients()); return 1; }
    int upto = argc > 3 ? atoi(argv[3]) : 8;
    uint32_t x = argc > 8 ? strtoul(argv[4], NULL, 0) : 200, y = argc > 8 ? strtoul(argv[5], NULL, 0) : 200;
    uint32_t w = argc > 8 ? strtoul(argv[6], NULL, 0) : 400, h = argc > 8 ? strtoul(argv[7], NULL, 0) : 200;
    uint32_t rgb = argc > 8 ? strtoul(argv[8], NULL, 0) : 0x0000C000;
    Firmware fw;
    printf("firmware:\n");
    if (!loadFirmware(argv[2], fw)) return 1;
    look(c);
    if (gFail || !discover(c, false)) return 1;
    c.area = (uint8_t*)be::map(c.areaPhys(), kAreaBytes, true);
    if (!c.area) return 1;
    // Our area must be memory: write, read back, clear.
    volatile uint32_t* probe = (volatile uint32_t*)(c.area + kDummyOff);
    probe[0] = 0x5A5AA5A5;
    be::flushWrites();
    if (probe[0] != 0x5A5AA5A5) { printf("STOP: our VRAM area does not hold data\n"); return 1; }
    memset(c.area, 0, kStoreOff);
    be::flushWrites();
    if (!be::arm(true)) { printf("STOP: could not arm the driver\n"); return 1; }
    bool ok = true;
    gOurIhBase = (uint32_t)((c.areaMc() + 0x40000) >> 8);
    if (ok && upto >= 1) { stepQuiesce(); ok = !gFail; }
    if (ok && upto >= 2) ok = stepGart(c);
    if (ok && upto >= 3) ok = stepRlc(c, fw);
    if (ok && upto >= 4) ok = stepCp(c, fw);
    if (ok && upto >= 5) ok = stepRing(c);
    if (ok && upto >= 6) ok = stepRingTest(c);
    if (ok && upto >= 7) ok = fill(c, x, y, w, h, rgb);
    if (ok && upto >= 8) ok = stepVm(c);
    printf("GRBM_STATUS 0x%08x CP_STAT 0x%08x fault status 0x%08x; refused register accesses: %u\n", R(GC(mmGRBM_STATUS)), R(GC(mmCP_STAT)),
        R(GC(mmGCVM_L2_PROTECTION_FAULT_STATUS)), be::refusedCount());
    be::arm(false);
#ifdef PS5GPU_MOCK
    be::report();
    if (ok && upto >= 7) {
        const uint32_t* px = (const uint32_t*)(be::vram() + (c.surfMc - c.mcBase));
        bool in = px[(uint64_t)y * c.pitchPx + x] == rgb && px[(uint64_t)(y + h - 1) * c.pitchPx + x + w - 1] == rgb;
        bool out = px[(uint64_t)y * c.pitchPx + x - 1] == 0 && px[(uint64_t)(y + h) * c.pitchPx + x] == 0 && px[(uint64_t)y * c.pitchPx + x + w] == 0;
        printf("MOCK: rectangle corners %s, neighbours %s\n", in ? "filled" : "NOT filled", out ? "untouched" : "TOUCHED");
        ok = ok && in && out;
    }
    ok = ok && be::clean();
#endif
    printf(ok ? "RESULT: OK up to step %d\n" : "RESULT: STOPPED (asked for step %d)\n", upto);
#ifdef PS5GPU_MOCK
    if (ok && upto >= 6 && getenv("MOCK_LIBTEST")) {
        ok = libtest(strtoul(getenv("MOCK_LIBTEST"), NULL, 0)) == 0;
        ok = libtest2(strtoul(getenv("MOCK_LIBTEST"), NULL, 0)) == 0 && ok;
        be::report();
        ok = ok && be::clean();
        printf(ok ? "MOCK LIBTEST: clean\n" : "MOCK LIBTEST: NOT clean\n");
    }
#endif
    return ok ? 0 : 1;
}
