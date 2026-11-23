// SPDX-License-Identifier: MIT
// GPU start, OS-free (see gpuboot.h). A port of tool/stage2.cpp's `run` (steps 1-6 and 8; 7 only for the mock
// comparison) that keeps stage2's register writes, in stage2's order, with stage2's values and timeouts. Only the
// surroundings differ: logging goes through env.line, memory is unmapped again, and the whole start has a time budget.
// Keep the two in step: build-gpuboot-mock.sh compares every register write of this file with stage2's on the model.
//
// Nothing here touches the PSP, the SMU, the display, PCI configuration space or the bus. RLC_PG_CNTL bit 23 is set
// so that the RLC sends no message to the SMU either (as stage2).
#include "gpuboot.h"
#include <string.h>
#include "CyanDirectLoad.hpp"

#include "../tool/regs.h"
#include "../tool/layout.h"
#include "clearstate_defs.h"
#include "clearstate_gfx10.h"

using namespace p5layout;

namespace gpuboot
{
const char* const kFwNames[kFwCount] = {
    "cyan_skillfish2_rlc.bin", "cyan_skillfish2_pfp.bin", "cyan_skillfish2_ce.bin", "cyan_skillfish2_me.bin",
    "cyan_skillfish2_mec.bin", "cyan_skillfish2_sdma.bin", "cyan_skillfish2_sdma1.bin",
};
}

namespace
{
using gpuboot::Env;

// 64-bit values are printed as two halves: the kernel's and the Mac's printf agree on nothing wider than %x.
#define H64(v) (unsigned)((uint64_t)(v) >> 32), (unsigned)(uint64_t)(v)

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

const Env* gEnv;
bool gFail;
uint32_t gOurIhBase;             // IH_RB_BASE value of ps5wddm's own interrupt ring (area + 0x40000)

void say(const char* fmt, ...)
{
    char line[240];
    va_list ap;
    va_start(ap, fmt);
    if (gEnv->vformat(line, sizeof line, fmt, ap) < 0) line[sizeof line - 1] = 0;
    va_end(ap);
    line[sizeof line - 1] = 0;
    gEnv->line(gEnv->ctx, line);
}

uint32_t R(uint32_t reg)
{
    uint32_t v = 0;
    if (!gEnv->rd(gEnv->ctx, reg, &v)) {
        say("REFUSED read 0x%05x", reg);
        gFail = true;
        return 0xFFFFFFFF;
    }
    return v;
}

void W(uint32_t reg, uint32_t value)
{
    if (!gEnv->wr(gEnv->ctx, reg, value)) {
        say("REFUSED write 0x%05x = 0x%08x", reg, value);
        gFail = true;
    }
}

void delayUs(uint32_t us) { gEnv->delayUs(gEnv->ctx, us); }
uint64_t nowUs() { return gEnv->nowUs(gEnv->ctx); }
void flushWrites() { gEnv->flushWrites(gEnv->ctx); }

uint32_t ioRead(void*, uint32_t reg) { return R(reg); }
void ioWrite(void*, uint32_t reg, uint32_t value) { W(reg, value); }
void ioDelay(void*, uint32_t us) { delayUs(us); }
const CyanDirectLoad::Io kIo = { nullptr, ioRead, ioWrite, ioDelay };

// Polls until (reg & mask) == want. Returns the microseconds it took, or -1.
int64_t waitReg(uint32_t reg, uint32_t mask, uint32_t want, uint32_t timeoutUs, uint32_t* last = nullptr)
{
    uint64_t start = nowUs();
    for (;;) {
        uint32_t v = R(reg);
        if (last) *last = v;
        if (gFail) return -1;
        if ((v & mask) == want) return (int64_t)(nowUs() - start);
        if (nowUs() - start > timeoutUs) return -1;
        delayUs(10);
    }
}

// ---- where things are (stage2 discover) ----

bool discover(Ctx& c)
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
    uint64_t surfRaw = ((uint64_t)(surfHi & 0xFFFF) << 32) | surfLo;
    c.pitchPx = (pitch & 0x3FFF) + 1;
    say("VRAM: GPU 0x%x%08x, %u MiB, physical 0x%x%08x, CONFIG_MEMSIZE %u MiB", H64(c.mcBase), (unsigned)(c.vramBytes >> 20), H64(c.carvePhys), memsize);
    if (c.carvePhys < 0x100000000ull || c.vramBytes < 0x40000000ull || c.vramBytes > 0x400000000ull || mcTop < c.mcBase) {
        say("STOP: VRAM layout not as expected (need a carve-out above 4 GiB of at least 1 GiB)");
        return false;
    }
    c.areaOff = c.vramBytes - kAreaBelowTop;
    if (surfRaw >= c.carvePhys && surfRaw < c.carvePhys + c.vramBytes) c.surfMc = c.mcBase + (surfRaw - c.carvePhys);
    else c.surfMc = surfRaw;
    bool surfOk = c.surfMc >= c.mcBase && c.surfMc + (uint64_t)c.pitchPx * 4 * 2160 <= c.mcBase + c.vramBytes && c.pitchPx >= 640 && c.pitchPx <= 8192;
    say("scan-out register 0x%x%08x -> GPU 0x%x%08x, pitch %u", H64(surfRaw), H64(c.surfMc), c.pitchPx);
    if (!surfOk) {
        say("note: scan-out registers not usable; no drawing");
        c.surfMc = 0;
    } else if (c.surfMc + (uint64_t)c.pitchPx * 4 * 2160 > c.areaMc() && c.surfMc < c.areaMc() + kAreaBytes) {
        say("STOP: the scan-out image overlaps our VRAM area");
        return false;
    }
    say("our area: GPU 0x%x%08x, physical 0x%x%08x", H64(c.areaMc()), H64(c.areaPhys()));
    return true;
}

// ---- step 1: interrupt rings off, engines halted (stage2 stepQuiesce) ----

void stepQuiesce()
{
    uint32_t t = R(OSS(mmIH_RB_CNTL));
    if (gOurIhBase && R(OSS(mmIH_RB_BASE)) == gOurIhBase && (t & IH_RB_CNTL__RB_ENABLE_MASK)) {
        say("step 1: interrupt ring 0 is the display driver's own: left running");
    } else {
        W(OSS(mmIH_RB_CNTL), t & ~(IH_RB_CNTL__RB_ENABLE_MASK | IH_RB_CNTL__ENABLE_INTR_MASK));
        W(OSS(mmIH_RB_RPTR), 0);
        W(OSS(mmIH_RB_WPTR), 0);
    }
    t = R(OSS(mmIH_RB_CNTL_RING1));
    W(OSS(mmIH_RB_CNTL_RING1), t & ~IH_RB_CNTL_RING1__RB_ENABLE_MASK);
    t = R(OSS(mmIH_RB_CNTL_RING2));
    W(OSS(mmIH_RB_CNTL_RING2), t & ~IH_RB_CNTL_RING2__RB_ENABLE_MASK);
    CyanDirectLoad::quiesce(kIo, true);
    say("step 1: interrupt rings off, CP_ME_CNTL 0x%08x CP_MEC_CNTL 0x%08x SDMA F32 0x%08x 0x%08x", R(GC(mmCP_ME_CNTL)), R(GC(mmCP_MEC_CNTL)),
        R(GC(mmSDMA0_F32_CNTL)), R(GC(mmSDMA1_F32_CNTL)));
    // A CP that hung inside a packet stays busy through a halt: gfx_v10_0_soft_reset's engine soft reset (not the bus or
    // full GPU reset of SAFETY.md), exactly as stage2.
    uint32_t stat = 0;
    if (waitReg(GC(mmCP_STAT), 0xFFFFFFFF, 0, 100000, &stat) < 0 && !gFail) {
        say("step 1: CP busy while halted (CP_STAT 0x%08x, GRBM_STATUS 0x%08x): CP soft reset", stat, R(GC(mmGRBM_STATUS)));
        W(GC(mmRLC_CNTL), R(GC(mmRLC_CNTL)) & ~RLC_CNTL__RLC_ENABLE_F32_MASK);
        uint32_t t2 = R(GC(mmGRBM_SOFT_RESET));
        W(GC(mmGRBM_SOFT_RESET), t2 | GRBM_SOFT_RESET__SOFT_RESET_CP_MASK);
        R(GC(mmGRBM_SOFT_RESET));
        delayUs(50);
        W(GC(mmGRBM_SOFT_RESET), t2 & ~GRBM_SOFT_RESET__SOFT_RESET_CP_MASK);
        R(GC(mmGRBM_SOFT_RESET));
        delayUs(50);
        const uint32_t pipelineBusy = GRBM_STATUS__PA_BUSY_MASK | GRBM_STATUS__SC_BUSY_MASK | GRBM_STATUS__BCI_BUSY_MASK | GRBM_STATUS__SX_BUSY_MASK |
                                      GRBM_STATUS__TA_BUSY_MASK | GRBM_STATUS__DB_BUSY_MASK | GRBM_STATUS__CB_BUSY_MASK | GRBM_STATUS__GDS_BUSY_MASK |
                                      GRBM_STATUS__SPI_BUSY_MASK | GRBM_STATUS__GE_BUSY_NO_DMA_MASK;
        uint32_t gs = R(GC(mmGRBM_STATUS));
        if (gs & pipelineBusy) {
            const uint32_t bits = GRBM_SOFT_RESET__SOFT_RESET_CP_MASK | GRBM_SOFT_RESET__SOFT_RESET_GFX_MASK;
            say("step 1: pipeline still busy (GRBM_STATUS 0x%08x): CP + GFX soft reset", gs);
            t2 = R(GC(mmGRBM_SOFT_RESET));
            W(GC(mmGRBM_SOFT_RESET), t2 | bits);
            R(GC(mmGRBM_SOFT_RESET));
            delayUs(50);
            W(GC(mmGRBM_SOFT_RESET), t2 & ~bits);
            R(GC(mmGRBM_SOFT_RESET));
            delayUs(50);
        }
        W(GC(mmCP_ME_CNTL), R(GC(mmCP_ME_CNTL)) | CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK | CP_ME_CNTL__CE_HALT_MASK);
        W(GC(mmCP_MEC_CNTL), R(GC(mmCP_MEC_CNTL)) | CP_MEC_CNTL__MEC_ME1_HALT_MASK | CP_MEC_CNTL__MEC_ME2_HALT_MASK);
        say("step 1: after the soft reset: CP_STAT 0x%08x GRBM_STATUS 0x%08x", R(GC(mmCP_STAT)), R(GC(mmGRBM_STATUS)));
    }
}

// ---- step 2: nothing may reach old Linux memory (stage2 flushTlb / stepGart) ----

bool flushTlb(uint32_t vmid)
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

bool stepGart(Ctx& c)
{
    uint64_t ptBase = (((uint64_t)R(GC(mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32)) << 32) | R(GC(mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32))) & ~0xFFFull;
    uint64_t start = R(GC(mmGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32)), end = R(GC(mmGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32));
    uint64_t bytes = (end - start + 1) * 8;
    if (ptBase < c.carvePhys || ptBase + bytes > c.carvePhys + c.vramBytes || bytes == 0 || bytes > 0x1000000 || end < start) {
        say("STOP step 2: VMID 0 page table 0x%x%08x (+0x%x) is not inside the carve-out", H64(ptBase), (unsigned)bytes);
        return false;
    }
    if (ptBase + bytes > c.areaPhys() && ptBase < c.areaPhys() + kAreaBytes) {
        say("STOP step 2: Linux's page table overlaps our VRAM area");
        return false;
    }
    uint64_t tablePhys = ptBase, mapPhys = tablePhys & ~0xFFFFull;
    uint64_t mapBytes = ((tablePhys + bytes + 0xFFFF) & ~0xFFFFull) - mapPhys;
    uint8_t* map = gEnv->mapVram(gEnv->ctx, mapPhys, mapBytes);
    if (!map) { say("STOP step 2: cannot map the GART table"); return false; }
    uint64_t* table = (uint64_t*)(map + (tablePhys - mapPhys));
    uint64_t valid = 0, first = 0;
    for (uint64_t i = 0; i < bytes / 8; ++i) {
        if (table[i] & 1) {
            if (!valid) first = table[i];
            ++valid;
        }
        table[i] = 0;
    }
    flushWrites();
    gEnv->unmapVram(gEnv->ctx, map, mapBytes);
    say("step 2: GART table at 0x%x%08x, %u entries, %u were valid (first 0x%x%08x); emptied", H64(ptBase), (unsigned)(bytes / 8), (unsigned)valid, H64(first));
    uint64_t dummyPhys = c.areaPhys() + kDummyOff;
    W(GC(mmGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32), (uint32_t)(dummyPhys >> 12));
    W(GC(mmGCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32), (uint32_t)(dummyPhys >> 44));
    bool ok = flushTlb(0);
    say("step 2: fault page now 0x%x%08x; VMID 0 TLB flush %s", H64(dummyPhys), ok ? "acknowledged" : "NOT acknowledged");
    return ok;
}

// ---- step 3: RLC (stage2 buildCsb / stepRlc) ----

struct Firmware {
    CyanDirectLoad::Image rlc, sdma0, sdma1;
    CyanDirectLoad::CpImages cp;
};

uint32_t buildCsb(uint32_t* b)
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

bool stepRlc(Ctx& c, const Firmware& fw)
{
    CyanDirectLoad::disableGpaMode(kIo);
    uint32_t t = R(GC(mmRLC_CNTL));
    W(GC(mmRLC_CNTL), t & ~RLC_CNTL__RLC_ENABLE_F32_MASK);
    W(GC(mmRLC_CGCG_CGLS_CTRL), 0);
    W(GC(mmRLC_PG_CNTL), 0);
    CyanDirectLoad::Report rep = CyanDirectLoad::loadRlc(kIo, fw.rlc);
    if (rep.status != CyanDirectLoad::Status::Ok || gFail) {
        say("STOP step 3: RLC load: %s", CyanDirectLoad::statusName(rep.status));
        return false;
    }
    uint32_t* csb = (uint32_t*)(c.area + kCsbOff);
    uint32_t dws = buildCsb(csb);
    flushWrites();
    uint64_t csbMc = c.areaMc() + kCsbOff;
    W(GC(mmRLC_CSIB_ADDR_HI), (uint32_t)(csbMc >> 32));
    W(GC(mmRLC_CSIB_ADDR_LO), (uint32_t)csbMc & 0xfffffffc);
    W(GC(mmRLC_CSIB_LENGTH), dws);
    uint32_t pre = R(GC(mmRLC_SPM_MC_CNTL));
    uint32_t data = (pre & ~RLC_SPM_MC_CNTL__RLC_SPM_VMID_MASK) | ((0xFu << RLC_SPM_MC_CNTL__RLC_SPM_VMID__SHIFT) & RLC_SPM_MC_CNTL__RLC_SPM_VMID_MASK);
    if (pre != data) W(GC(mmRLC_SPM_MC_CNTL), data);
    // RLC_PG_CNTL[23] = 1: "RLC will not issue any message to SMU".
    W(GC(mmRLC_PG_CNTL), R(GC(mmRLC_PG_CNTL)) | 0x800000);
    W(GC(mmRLC_CNTL), R(GC(mmRLC_CNTL)) | RLC_CNTL__RLC_ENABLE_F32_MASK);
    delayUs(50);
    say("step 3: RLC loaded (%u dwords), clear-state %u dwords, RLC_CNTL 0x%08x RLC_PG_CNTL 0x%08x", rep.dataWrites, dws, R(GC(mmRLC_CNTL)), R(GC(mmRLC_PG_CNTL)));
    return !gFail;
}

// ---- step 4: CP and SDMA microcode (stage2 stepCp) ----

bool stepCp(Ctx& c, const Firmware& fw)
{
    CyanDirectLoad::Store store = { c.area + kStoreOff, c.areaMc() + kStoreOff, kStoreBytes };
    uint64_t need = CyanDirectLoad::storeBytesNeeded(fw.cp);
    CyanDirectLoad::Status st = CyanDirectLoad::stageCp(fw.cp, store);
    if (st != CyanDirectLoad::Status::Ok) {
        say("STOP step 4: CP store (need 0x%x bytes): %s", (unsigned)need, CyanDirectLoad::statusName(st));
        return false;
    }
    CyanDirectLoad::Report rep = CyanDirectLoad::loadCp(kIo, fw.cp, store);
    flushWrites();
    say("step 4: CP load: %s (step %u), CP_STAT after halt 0x%x (%u us), icache waits %u %u %u %u us", CyanDirectLoad::statusName(rep.status), rep.step,
        rep.cpStatAfterHalt, rep.haltWaitUs, rep.icacheWaitUs[0], rep.icacheWaitUs[1], rep.icacheWaitUs[2], rep.icacheWaitUs[3]);
    if (rep.status != CyanDirectLoad::Status::Ok || gFail) return false;
    if (!CyanDirectLoad::storeIntact(fw.cp, store)) {
        say("STOP step 4: the CP store does not read back");
        return false;
    }
    CyanDirectLoad::Report sd = CyanDirectLoad::loadSdma(kIo, fw.sdma0, fw.sdma1);
    say("step 4: SDMA load: %s (%u dwords)", CyanDirectLoad::statusName(sd.status), sd.dataWrites);
    return sd.status == CyanDirectLoad::Status::Ok && !gFail;
}

// ---- step 5: graphics ring (stage2 ringWrite / ringBegin / ringCommit / ringDrain / stepRing) ----

void ringWrite(Ctx& c, uint32_t v)
{
    c.ring[c.wptr & (c.ringDw - 1)] = v;
    ++c.wptr;
}

void ringBegin(Ctx& c, uint32_t dwords)
{
    uint32_t off = (uint32_t)(c.wptr & (c.ringDw - 1));
    if (off + dwords > c.ringDw)
        while (c.wptr & (c.ringDw - 1)) ringWrite(c, PACKET3(PACKET3_NOP, 0x3FFF));
}

void ringCommit(Ctx& c)
{
    while (c.wptr & 0xFF) ringWrite(c, PACKET3(PACKET3_NOP, 0x3FFF));
    *(volatile uint64_t*)(c.area + kWptrOff) = c.wptr;
    ((Shared*)(c.area + kSharedOff))->wptr = c.wptr;
    flushWrites();
    if (!gEnv->doorbell64(gEnv->ctx, kDoorbellIndex * 4, c.wptr)) {
        say("doorbell write failed");
        gFail = true;
    }
}

bool ringDrain(Ctx& c, uint32_t timeoutUs, const char* what)
{
    uint32_t last = 0;
    int64_t us = waitReg(GC(mmCP_RB0_RPTR), 0xFFFFFFFF, (uint32_t)(c.wptr & (c.ringDw - 1)), timeoutUs, &last);
    if (us < 0) {
        say("%s: the CP did not consume the ring: rptr 0x%x, wptr 0x%x, CP_STAT 0x%08x, GRBM_STATUS 0x%08x, fault status 0x%08x", what ? what : "ring", last,
            (uint32_t)(c.wptr & (c.ringDw - 1)), R(GC(mmCP_STAT)), R(GC(mmGRBM_STATUS)), R(GC(mmGCVM_L2_PROTECTION_FAULT_STATUS)));
        return false;
    }
    if (what) say("%s: ring consumed in %u us", what, (unsigned)us);
    return true;
}

uint32_t gCsb[4096];

bool stepRing(Ctx& c)
{
    c.ring = (uint32_t*)(c.area + kRingOff);
    c.wptr = 0;
    Shared* shared = (Shared*)(c.area + kSharedOff);
    shared->magic = 0;
    shared->version = 1;
    shared->wptr = 0;
    shared->seq = 0;
    uint64_t ringMc = c.areaMc() + kRingOff, rptrMc = c.areaMc() + kRptrOff, wptrMc = c.areaMc() + kWptrOff;
    W(GC(mmCP_RB_WPTR_DELAY), 0);
    W(GC(mmCP_RB_VMID), 0);
    W(GC(mmGRBM_GFX_CNTL), R(GC(mmGRBM_GFX_CNTL)) & ~GRBM_GFX_CNTL__PIPEID_MASK);
    uint32_t bufsz = 0;
    while ((1u << bufsz) < kRingBytes / 8) ++bufsz;
    uint32_t cntl = (bufsz << CP_RB0_CNTL__RB_BUFSZ__SHIFT) | ((bufsz - 2) << CP_RB0_CNTL__RB_BLKSZ__SHIFT) |
                    (2u << CP_RB0_CNTL__MIN_AVAILSZ__SHIFT) | (2u << CP_RB0_CNTL__MIN_IB_AVAILSZ__SHIFT);
    W(GC(mmCP_RB0_CNTL), cntl);
    W(GC(mmCP_RB0_WPTR), 0);
    W(GC(mmCP_RB0_WPTR_HI), 0);
    W(GC(mmCP_RB0_RPTR_ADDR), (uint32_t)rptrMc);
    W(GC(mmCP_RB0_RPTR_ADDR_HI), (uint32_t)(rptrMc >> 32) & CP_RB_RPTR_ADDR_HI__RB_RPTR_ADDR_HI_MASK);
    W(GC(mmCP_RB_WPTR_POLL_ADDR_LO), (uint32_t)wptrMc);
    W(GC(mmCP_RB_WPTR_POLL_ADDR_HI), (uint32_t)(wptrMc >> 32));
    delayUs(1000);
    W(GC(mmCP_RB0_CNTL), cntl);
    uint64_t rbAddr = ringMc >> 8;
    W(GC(mmCP_RB0_BASE), (uint32_t)rbAddr);
    W(GC(mmCP_RB0_BASE_HI), (uint32_t)(rbAddr >> 32));
    W(GC(mmCP_RB_ACTIVE), 1);
    uint32_t quantum = R(GC(mmCP_GFX_HQD_QUANTUM));
    W(GC(mmCP_GFX_HQD_QUANTUM), 0);
    say("step 5: queue time slice was 0x%x, now 0x%x", quantum, R(GC(mmCP_GFX_HQD_QUANTUM)));
    uint32_t db = R(GC(mmCP_RB_DOORBELL_CONTROL));
    db = (db & ~CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) | ((kDoorbellIndex << CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT) & CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK);
    db |= CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK;
    W(GC(mmCP_RB_DOORBELL_CONTROL), db);
    W(GC(mmCP_RB_DOORBELL_RANGE_LOWER), (kDoorbellIndex << CP_RB_DOORBELL_RANGE_LOWER__DOORBELL_RANGE_LOWER__SHIFT) & CP_RB_DOORBELL_RANGE_LOWER__DOORBELL_RANGE_LOWER_MASK);
    W(GC(mmCP_RB_DOORBELL_RANGE_UPPER), CP_RB_DOORBELL_RANGE_UPPER__DOORBELL_RANGE_UPPER_MASK);
    W(GC(mmCP_MAX_CONTEXT), 8 - 1);
    W(GC(mmCP_DEVICE_ID), 1);
    uint32_t t = R(GC(mmCP_ME_CNTL));
    W(GC(mmCP_ME_CNTL), t & ~(CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK | CP_ME_CNTL__CE_HALT_MASK));
    uint32_t stat = 0;
    int64_t us = waitReg(GC(mmCP_STAT), 0xFFFFFFFF, 0, 100000, &stat);
    say("step 5: ring at GPU 0x%x%08x; CP unhalted, CP_ME_CNTL 0x%08x, CP_STAT 0x%08x (%s)", H64(ringMc), R(GC(mmCP_ME_CNTL)), stat,
        us >= 0 ? "idle" : "not idle after 100 ms; Linux only warns here");
    if (gFail) return false;
    uint32_t n = buildCsb(gCsb);
    if (n + 4 + 256 > c.ringDw) { say("STOP step 5: the clear-state stream does not fit the ring"); return false; }
    for (uint32_t i = 0; i < n; ++i) ringWrite(c, gCsb[i]);
    ringWrite(c, PACKET3(PACKET3_SET_BASE, 2));
    ringWrite(c, PACKET3_BASE_INDEX(CE_PARTITION_BASE));
    ringWrite(c, 0x8000);
    ringWrite(c, 0x8000);
    ringCommit(c);
    return ringDrain(c, 500000, "step 5 (clear state)");
}

// ---- step 6: ring test (stage2 stepRingTest) ----

bool stepRingTest(Ctx& c)
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
        say("step 6: RING TEST FAILED: scratch register reads 0x%08x", last);
        ringDrain(c, 1000, "step 6");
        return false;
    }
    say("step 6: RING TEST PASSED in %u us", (unsigned)us);
    ((Shared*)(c.area + kSharedOff))->magic = kSharedMagic;   // from here on the library may attach
    *(volatile uint64_t*)(c.area + kFenceOff) = 0;
    *(volatile uint32_t*)(c.area + 0xA00000) = 0;             // the library's state described the old ring: start it afresh
    flushWrites();
    return true;
}

// ---- step 7: a rectangle drawn by the GPU (stage2 fill; only for the comparison on the model) ----

bool fill(Ctx& c, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour)
{
    if (!c.surfMc) { say("no scan-out address; not drawing"); return false; }
    if (w == 0 || h == 0 || x + w > c.pitchPx || y + h > 1080 || h > 600) { say("rectangle out of range"); return false; }
    uint64_t rowPhys = c.carvePhys + (c.surfMc - c.mcBase) + ((uint64_t)y * c.pitchPx + x) * 4;
    uint64_t mapPhys = rowPhys & ~0xFFFFull, mapBytes = (((uint64_t)h * c.pitchPx * 4 + (rowPhys - mapPhys)) + 0xFFFF) & ~0xFFFFull;
    uint8_t* map = gEnv->mapVram(gEnv->ctx, mapPhys, mapBytes);
    if (!map) return false;
    volatile uint32_t* px = (volatile uint32_t*)(map + (rowPhys - mapPhys));
    for (uint32_t row = 0; row < h; ++row) {
        uint64_t dst = c.surfMc + ((uint64_t)(y + row) * c.pitchPx + x) * 4;
        if (row && row % 32 == 0) {
            ringCommit(c);
            if (!ringDrain(c, 500000, nullptr)) { gEnv->unmapVram(gEnv->ctx, map, mapBytes); return false; }
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
    bool ok = ringDrain(c, 500000, nullptr);
    if (ok) {
        ringWrite(c, PACKET3(PACKET3_SET_UCONFIG_REG, 1));
        ringWrite(c, scratch - PACKET3_SET_UCONFIG_REG_START);
        ringWrite(c, 0x600DF111);
        ringCommit(c);
        int64_t us = waitReg(scratch, 0xFFFFFFFF, 0x600DF111, 500000);
        uint32_t after = px[0], afterLast = px[(uint64_t)(h - 1) * c.pitchPx + (w - 1)];
        ok = us >= 0 && after == colour && afterLast == colour;
        say("step 7: fill %ux%u at %u,%u: %s", w, h, x, y, ok ? "THE GPU DREW IT" : "not drawn");
        if (us < 0) ringDrain(c, 1000, "step 7");
    }
    gEnv->unmapVram(gEnv->ctx, map, mapBytes);
    return ok;
}

// ---- step 8: a user address space (VMID 1), an indirect buffer and a fence (stage2 stepVm) ----

const uint64_t kUserVa = 0x100000000ull;
const uint64_t kPteValid = 1, kPteExecutable = 1 << 4, kPteReadable = 1 << 5, kPteWriteable = 1 << 6;

bool stepVm(Ctx& c)
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
    flushWrites();
    W(GC(mmGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32), (uint32_t)(ptPhys | kPteValid));
    W(GC(mmGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_HI32), (uint32_t)(ptPhys >> 32));
    if (!flushTlb(1)) { say("STOP step 8: VMID 1 TLB flush not acknowledged"); return false; }

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
    flushWrites();
    W(scratch, 0);
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
    uint64_t start = nowUs();
    while (*fence != seq && nowUs() - start < 500000) delayUs(20);
    uint64_t took = nowUs() - start;
    volatile uint32_t* out = (volatile uint32_t*)(c.area + kUserOff + 0x10000);
    bool fenceOk = *fence == seq, markOk = R(scratch) == mark, fillOk = out[0] == pattern && out[255] == pattern && out[256] == 0;
    say("step 8: fence %s (0x%08x after %u us), scratch %s, buffer %s, fault status 0x%08x", fenceOk ? "signalled" : "NOT signalled", (uint32_t)*fence,
        (unsigned)took, markOk ? "ok" : "wrong", fillOk ? "filled through the user mapping" : "wrong", R(GC(mmGCVM_L2_PROTECTION_FAULT_STATUS)));
    if (!fenceOk) ringDrain(c, 1000, "step 8");
    return fenceOk && markOk && fillOk;
}

void setReason(char* out, size_t bytes, const char* fmt, ...)
{
    if (!out || !bytes) return;
    va_list ap;
    va_start(ap, fmt);
    if (gEnv->vformat(out, bytes, fmt, ap) < 0) out[bytes - 1] = 0;
    va_end(ap);
    out[bytes - 1] = 0;
}
} // namespace

namespace gpuboot
{
Result run(const Env& env, const Files& files, const Options& opt, char* reason, size_t reasonBytes)
{
    gEnv = &env;
    gFail = false;
    gOurIhBase = 0;
    if (reason && reasonBytes) reason[0] = 0;
    const uint64_t t0 = nowUs();
    Firmware fw;
    memset(&fw, 0, sizeof fw);
    {
        using CyanDirectLoad::Kind;
        CyanDirectLoad::Image* images[kFwCount] = { &fw.rlc, &fw.cp.pfp, &fw.cp.ce, &fw.cp.me, &fw.cp.mec, &fw.sdma0, &fw.sdma1 };
        const Kind kinds[kFwCount] = { Kind::Rlc, Kind::CpGfx, Kind::CpGfx, Kind::CpGfx, Kind::CpMec, Kind::Sdma, Kind::Sdma };
        for (int i = 0; i < kFwCount; ++i) {
            if (!files.data[i] || !CyanDirectLoad::parse(files.data[i], files.bytes[i], kinds[i], *images[i])) {
                setReason(reason, reasonBytes, "firmware file %s not as expected", kFwNames[i]);
                say("STOP: %s", reason);
                return kBadFirmware;
            }
            say("%s: version 0x%x, %u bytes of code", kFwNames[i], images[i]->version, images[i]->codeBytes);
        }
    }
    Ctx c;
    if (!discover(c)) {
        setReason(reason, reasonBytes, gFail ? "a register read was refused" : "GPU memory layout not as expected");
        return gFail ? kRefused : kLayout;
    }
    {
        CyanDirectLoad::Snapshot s;
        CyanDirectLoad::snapshot(kIo, s);
        say("found: RLC_CNTL 0x%08x GRBM_STATUS 0x%08x CP_ME_CNTL 0x%08x CP_MEC_CNTL 0x%08x CP_STAT 0x%08x IH_RB_CNTL 0x%08x ring base 0x%08x", s.rlcCntl,
            s.grbmStatus, s.cpMeCntl, s.cpMecCntl, s.cpStat, R(OSS(mmIH_RB_CNTL)), R(GC(mmCP_RB0_BASE)));
    }
    c.area = env.mapVram(env.ctx, c.areaPhys(), kAreaBytes);
    if (!c.area) {
        setReason(reason, reasonBytes, "our GPU memory area cannot be mapped");
        return kNoArea;
    }
    Result r = kOk;
    volatile uint32_t* probe = (volatile uint32_t*)(c.area + kDummyOff);
    probe[0] = 0x5A5AA5A5;
    flushWrites();
    if (probe[0] != 0x5A5AA5A5) {
        setReason(reason, reasonBytes, "our GPU memory area does not hold data");
        r = kNoArea;
    }
    // libps5gpu's state (area + 10 MiB): "P5S3" and live clients. Only asked for a restart in a running Windows; at
    // boot nothing can be attached and what is there is left over from an earlier session.
    if (r == kOk && opt.refuseIfClients && *(volatile uint32_t*)(c.area + 0xA00000) == 0x33533550u && *(volatile uint32_t*)(c.area + 0xA00004) != 0) {
        setReason(reason, reasonBytes, "%u program(s) are using the GPU", *(volatile uint32_t*)(c.area + 0xA00004));
        r = kClients;
    }
    if (r != kOk) {
        say("STOP: %s", reason);
        env.unmapVram(env.ctx, c.area, kAreaBytes);
        return r;
    }
    memset(c.area, 0, kStoreOff);
    flushWrites();
    gOurIhBase = (uint32_t)((c.areaMc() + 0x40000) >> 8);

    struct Step { uint32_t n; const char* name; Result fail; } const steps[] = {
        { 1, "quiesce", kRefused }, { 2, "gart", kGart }, { 3, "rlc", kRlc }, { 4, "cp+sdma", kCp }, { 5, "ring", kRing },
        { 6, "ring test", kRingTest }, { 7, "fill", kFill }, { 8, "vm", kVm },
    };
    for (const Step& s : steps) {
        if (s.n == 7 && !opt.drawTest) continue;
        uint64_t used = (nowUs() - t0) / 1000;
        if (opt.budgetMs && used >= opt.budgetMs) {
            setReason(reason, reasonBytes, "time budget used up before step %u (%u ms of %u ms)", s.n, (unsigned)used, opt.budgetMs);
            r = kTimeout;
            break;
        }
        env.step(env.ctx, s.n, s.name);
        bool ok = false;
        switch (s.n) {
        case 1: stepQuiesce(); ok = !gFail; break;
        case 2: ok = stepGart(c); break;
        case 3: ok = stepRlc(c, fw); break;
        case 4: ok = stepCp(c, fw); break;
        case 5: ok = stepRing(c); break;
        case 6: ok = stepRingTest(c); break;
        case 7: ok = fill(c, 200, 200, 400, 200, 0x0000C000); break;
        case 8: ok = stepVm(c); break;
        }
        if (!ok) {
            setReason(reason, reasonBytes, "step %u (%s) failed%s", s.n, s.name, gFail ? " (register access refused)" : "");
            r = gFail ? kRefused : s.fail;
            break;
        }
    }
    say("GRBM_STATUS 0x%08x CP_STAT 0x%08x fault status 0x%08x; %u ms", R(GC(mmGRBM_STATUS)), R(GC(mmCP_STAT)), R(GC(mmGCVM_L2_PROTECTION_FAULT_STATUS)),
        (unsigned)((nowUs() - t0) / 1000));
    env.unmapVram(env.ctx, c.area, kAreaBytes);
    if (r == kOk) setReason(reason, reasonBytes, "GPU started (ring test and command-buffer test passed) in %u ms", (unsigned)((nowUs() - t0) / 1000));
    say("%s: %s", r == kOk ? "RESULT OK" : "RESULT STOPPED", reason);
    return r;
}
} // namespace gpuboot
