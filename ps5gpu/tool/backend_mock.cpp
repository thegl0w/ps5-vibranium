// SPDX-License-Identifier: MIT
// Host model of the GPU for testing stage2.cpp on the Mac: registers as a table preset to what Linux leaves on the
// console, VRAM as sparse memory, the driver's firewall, and a small command-processor simulator that runs the ring
// when the write pointer moves. It checks our packet stream and pointer arithmetic, not the hardware.
#include <sys/mman.h>
#include <time.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include "backend.h"
#include "regs.h"
#include "layout.h"

static const uint64_t kCarvePhys = 0x370000000ull, kVramBytes = 0x100000000ull, kMcBase = 0xF400000000ull;
static std::map<uint32_t, uint32_t> gReg;
static uint8_t* gVram;
static bool gArmed;
static uint32_t gRefused, gReads, gWrites, gRptr, gPackets, gFills, gBadPackets, gIbs, gFlushes;
static uint64_t gFakeUs, gDoorbellSeen;
static const uint32_t kRingMask = (uint32_t)(p5layout::kRingBytes / 4) - 1;
static FILE* gTrace;

enum : uint32_t {
    rGrbmStatus = GC(mmGRBM_STATUS), rCpStat = GC(mmCP_STAT), rCpMeCntl = GC(mmCP_ME_CNTL),
    rCpRb0Base = GC(mmCP_RB0_BASE), rCpRb0BaseHi = GC(mmCP_RB0_BASE_HI), rCpRb0Rptr = GC(mmCP_RB0_RPTR), rCpRb0Wptr = GC(mmCP_RB0_WPTR),
    rInvReq17 = GC(mmGCVM_INVALIDATE_ENG17_REQ), rInvAck17 = GC(mmGCVM_INVALIDATE_ENG17_ACK),
};

static bool forbidden(uint32_t reg, bool write, uint32_t value)
{
    auto smnMp = [](uint32_t s) { return (s >= 0x03800000 && s <= 0x03BFFFFF) || (s >= 0x58000 && s <= 0x5FFFF); };
    auto mmMp = [](uint32_t i) { return !(i & 0x80000000u) && (i >> 2) >= 0x16000 && (i >> 2) <= 0x17FFF; };
    if ((uint64_t)reg * 4 + 4 > 0x80000) return true;
    if (reg >= 0x16000 && reg <= 0x17FFF) return true;
    if (reg == 0 && write && mmMp(value)) return true;
    if (reg == 1 && mmMp(gReg[0])) return true;
    if ((reg == 0xC || reg == 0xE) && write && smnMp(value)) return true;
    if (reg == 0xD && smnMp(gReg[0xC])) return true;
    if (reg == 0xF && smnMp(gReg[0xE])) return true;
    return write && !gArmed;
}

static uint8_t* mc(uint64_t addr, uint64_t bytes)
{
    if (addr < kMcBase || addr + bytes > kMcBase + kVramBytes) return nullptr;
    return gVram + (addr - kMcBase);
}

static uint8_t* phys(uint64_t addr, uint64_t bytes)
{
    if (addr < kCarvePhys || addr + bytes > kCarvePhys + kVramBytes) return nullptr;
    return gVram + (addr - kCarvePhys);
}

// A GPU address as the CP sees it: VMID 0 = the FB range directly; other VMIDs through their page tables (four levels,
// entries = physical address | flags, as stage2.cpp builds them).
static uint8_t* gpuAddr(uint32_t vmid, uint64_t va, uint64_t bytes, bool write)
{
    if (vmid == 0) return mc(va, bytes);
    uint64_t base = ((uint64_t)gReg[GC(mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32) + 2 * vmid] << 32) | gReg[GC(mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32) + 2 * vmid];
    if (!(base & 1) || (va & 0xFFF) + bytes > 0x1000) return nullptr;
    uint64_t table = base & ~0xFFFull;
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t* t = (uint64_t*)phys(table, 0x1000);
        if (!t) return nullptr;
        uint64_t e = t[(va >> shift) & 511];
        if (!(e & 1)) return nullptr;
        table = e & 0x0000FFFFFFFFF000ull;
        if (shift == 12 && !(e & (write ? 0x40 : 0x20))) return nullptr;   // writeable / readable
    }
    return phys(table + (va & 0xFFF), bytes);
}

static void execPackets(uint32_t* p, uint32_t count, uint32_t vmid, int depth)
{
    uint32_t i = 0;
    while (i < count) {
        uint32_t h = p[i];
        if ((h >> 30) != 3) { printf("MOCK: not a type-3 packet (0x%08x) in vmid %u\n", h, vmid); ++gBadPackets; return; }
        uint32_t op = (h >> 8) & 0xFF, n = (h >> 16) & 0x3FFF, body = n == 0x3FFF ? 0 : n + 1;
        uint32_t* b = &p[i + 1];
        if (i + 1 + body > count) { printf("MOCK: packet 0x%02x runs past the end\n", op); ++gBadPackets; return; }
        ++gPackets;
        switch (op) {
        case 0x10: case 0x4A: case 0x28: case 0x12: case 0x11: case 0x69: case 0x42: break;   // NOP, PREAMBLE, CONTEXT_CONTROL, CLEAR_STATE, SET_BASE, SET_CONTEXT_REG, PFP_SYNC_ME
        case 0x79:                                                                              // SET_UCONFIG_REG
            for (uint32_t k = 1; k < body; ++k) gReg[PACKET3_SET_UCONFIG_REG_START + b[0] + k - 1] = b[k];
            break;
        case 0x50: {                                                                            // DMA_DATA
            uint64_t dst = ((uint64_t)b[4] << 32) | b[3];
            uint32_t bytes = b[5] & 0x03FFFFFF, srcSel = (b[0] >> 29) & 3;
            bool ok = body == 6 && srcSel == 2 && !(bytes & 3);
            for (uint32_t k = 0; ok && k < bytes / 4; ++k) {
                uint8_t* q = gpuAddr(vmid, dst + k * 4, 4, true);
                if (!q) { ok = false; break; }
                memcpy(q, &b[1], 4);
            }
            if (!ok) { printf("MOCK: bad DMA_DATA in vmid %u (dst 0x%llx, %u bytes)\n", vmid, (unsigned long long)dst, bytes); ++gBadPackets; }
            else ++gFills;
            break;
        }
        case 0x3F: case 0x33: {                                                                 // INDIRECT_BUFFER, INDIRECT_BUFFER_CNST
            uint64_t va = ((uint64_t)b[1] << 32) | b[0];
            uint32_t dws = b[2] & 0xFFFFF, ibVmid = (b[2] >> 24) & 0xF;
            std::vector<uint32_t> ib(dws);
            bool ok = body == 3 && depth == 0 && dws != 0;
            for (uint32_t k = 0; ok && k < dws; ++k) {
                uint8_t* q = gpuAddr(ibVmid, va + k * 4ull, 4, false);
                if (!q) ok = false; else memcpy(&ib[k], q, 4);
            }
            if (!ok) { printf("MOCK: bad INDIRECT_BUFFER (va 0x%llx, %u dwords, vmid %u)\n", (unsigned long long)va, dws, ibVmid); ++gBadPackets; break; }
            ++gIbs;
            execPackets(ib.data(), dws, ibVmid, depth + 1);
            break;
        }
        case 0x49: {                                                                            // RELEASE_MEM
            uint64_t addr = ((uint64_t)b[3] << 32) | b[2];
            uint32_t sel = (b[1] >> 29) & 7, bytes = sel == 2 ? 8 : 4;
            uint8_t* q = mc(addr, bytes);
            if (body != 7 || !q || (sel != 1 && sel != 2)) { printf("MOCK: bad RELEASE_MEM (addr 0x%llx, sel %u)\n", (unsigned long long)addr, sel); ++gBadPackets; break; }
            memcpy(q, &b[4], bytes);
            break;
        }
        case 0x37: {                                                                            // WRITE_DATA to a register
            if (body != 4 || ((b[0] >> 8) & 0xF) != 0 || b[2] != 0 || forbidden(b[1], false, 0)) { printf("MOCK: bad WRITE_DATA (ctl 0x%x reg 0x%x)\n", b[0], b[1]); ++gBadPackets; break; }
            gReg[b[1]] = b[3];
            if (b[1] == GC(mmGCVM_INVALIDATE_ENG0_REQ)) { gReg[GC(mmGCVM_INVALIDATE_ENG0_ACK)] = b[3] & 0xFFFF; ++gFlushes; }
            break;
        }
        case 0x58:                                                                              // ACQUIRE_MEM (cache write-back/invalidate): nothing to model
            if (body != 7) { printf("MOCK: bad ACQUIRE_MEM\n"); ++gBadPackets; }
            break;
        case 0x3C: {                                                                            // WAIT_REG_MEM, register or memory ("equal")
            if (body == 6 && (b[0] & 0x10)) {                                                   // memory: the ring runs in order here, so it must already hold
                uint64_t addr = ((uint64_t)b[2] << 32) | b[1];
                uint32_t* q = (addr & 3) ? nullptr : (uint32_t*)mc(addr, 4);
                if ((b[0] & 7) != 3 || !q || (*q & b[4]) != b[3]) { printf("MOCK: WAIT_REG_MEM on memory would hang (addr 0x%llx = 0x%x, want 0x%x)\n", (unsigned long long)addr, q ? *q : 0, b[3]); ++gBadPackets; }
                break;
            }
            if (body != 6 || (b[0] & 0x10) || (gReg[b[1]] & b[4]) != b[3]) { printf("MOCK: WAIT_REG_MEM would hang (reg 0x%x = 0x%x, want 0x%x mask 0x%x)\n", b[1], gReg[b[1]], b[3], b[4]); ++gBadPackets; }
            break;
        }
        default:
            printf("MOCK: unexpected packet opcode 0x%02x\n", op);
            ++gBadPackets;
        }
        i += 1 + body;
    }
}

// Runs the ring from the simulated read pointer to the new write pointer. Our commits never wrap inside a packet.
static void runRing(uint32_t wptr)
{
    if (gReg[rCpMeCntl] & 0x15000000) return;                       // halted
    uint64_t base = (((uint64_t)gReg[rCpRb0BaseHi] << 32) | gReg[rCpRb0Base]) << 8;
    uint32_t* ring = (uint32_t*)mc(base, p5layout::kRingBytes);
    if (!ring) { printf("MOCK: ring base 0x%llx is not in VRAM\n", (unsigned long long)base); ++gBadPackets; return; }
    std::vector<uint32_t> seg;
    for (uint32_t p = gRptr; p != wptr; p = (p + 1) & kRingMask) seg.push_back(ring[p]);
    // Like the console: a packet that straddles the end of the ring hangs the CP.
    for (uint32_t i = 0, p = gRptr; i < seg.size();) {
        uint32_t n = (seg[i] >> 16) & 0x3FFF, len = 1 + (n == 0x3FFF ? 0 : n + 1);
        if (p + len > kRingMask + 1) { printf("MOCK: packet 0x%08x at ring offset 0x%x straddles the end of the ring\n", seg[i], p); ++gBadPackets; break; }
        i += len;
        p = (p + len) & kRingMask;
    }
    execPackets(seg.data(), (uint32_t)seg.size(), 0, 0);
    gRptr = wptr;
}

namespace be
{
    static uint32_t gDeadPid;
    void lock() {}
    bool lockReady() { return true; }
    bool envFlag(const char* name) { const char* v = getenv(name); return v && strcmp(v, "0") != 0; }
    void unlock() {}
    static uint32_t gLastPid = 100;
    uint32_t pid() { return ++gLastPid; }                         // every p5_open looks like another program
    uint32_t lastPid() { return gLastPid; }
    bool pidAlive(uint32_t pid) { return pid != gDeadPid; }
    bool isCompositor() { return getenv("PS5GPU_MOCK_DWM") != nullptr; }
    uint64_t bootStamp() { return 1; }
    void killPid(uint32_t pid) { gDeadPid = pid; }

    bool open()
    {
        if (gVram) return true;
        gVram = (uint8_t*)mmap(nullptr, kVramBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (gVram == MAP_FAILED) { printf("MOCK: cannot reserve VRAM\n"); return false; }
        gTrace = fopen(getenv("MOCK_TRACE") ? getenv("MOCK_TRACE") : "/dev/null", "w");
        // What Linux leaves on the console (4 GiB carve-out; scan-out 2560 wide at VRAM offset 0x1A000000; GART table in
        // the top megabyte with every entry valid; engines running).
        gReg[GC(mmGCMC_VM_FB_LOCATION_BASE)] = 0x00F400;
        gReg[GC(mmGCMC_VM_FB_LOCATION_TOP)] = 0x00F4FF;
        gReg[GC(mmGCMC_VM_FB_OFFSET)] = 0x370;
        gReg[NBIO(mmRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE)] = 4096;
        gReg[DMU(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS)] = 0x8A000000;     // physical 0x3_8A000000
        gReg[DMU(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH)] = 0x3;
        gReg[DMU(mmHUBPREQ0_DCSURF_SURFACE_PITCH)] = 2559;
        gReg[DMU(mmHUBPREQ3_DCSURF_PRIMARY_SURFACE_ADDRESS)] = 0x8A000000;
        gReg[DMU(mmHUBPREQ3_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH)] = 0x3;
        gReg[DMU(mmHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION)] = (1440u << 16) | 1280;
        gReg[DMU(mmHUBP3_DCSURF_PRI_VIEWPORT_DIMENSION)] = (1440u << 16) | 1280;
        gReg[DMU(mmHUBP3_DCSURF_PRI_VIEWPORT_START)] = 1280;
        gReg[DMU(mmHUBP0_DCHUBP_CNTL)] = 0x000f0102;
        gReg[DMU(mmHUBP1_DCHUBP_CNTL)] = 0x000f100b;
        gReg[DMU(mmHUBP2_DCHUBP_CNTL)] = 0x000f100b;
        gReg[DMU(mmHUBP3_DCHUBP_CNTL)] = 0x000e0100;
        gReg[GC(mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32)] = 0x6FF00001;    // physical 0x4_6FF00000, valid
        gReg[GC(mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32)] = 0x4;
        gReg[GC(mmGCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32)] = 0;
        gReg[GC(mmGCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32)] = 0x1FFFF;
        for (uint64_t i = 0; i < 0x20000; ++i) ((uint64_t*)(gVram + 0xFFF00000))[i] = 0x100000000ull + i * 0x1000 + 0x73;
        gReg[GC(mmRLC_CNTL)] = 1;
        gReg[OSS(mmIH_RB_CNTL)] = 0x403301a1;
        gReg[GC(mmRLC_PG_CNTL)] = 0x8;
        gReg[GC(mmCP_RB_DOORBELL_CONTROL)] = 0xc0000458;
        return true;
    }

    bool arm(bool on) { gArmed = on; return true; }

    bool rd(uint32_t reg, uint32_t& value)
    {
        value = 0;
        if (forbidden(reg, false, 0)) { ++gRefused; return false; }
        ++gReads;
        if (reg == rCpStat || reg == rGrbmStatus) value = 0;
        else if (reg == rCpRb0Rptr) value = gRptr;
        else if (reg == rInvAck17) value = gReg[rInvReq17] & 0xFFFF;
        else if (reg == DMU(mmHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE)) value = gReg[DMU(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS)];
        else if (reg == DMU(mmHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH)) value = gReg[DMU(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH)];
        else if (reg == DMU(mmHUBPREQ3_DCSURF_SURFACE_EARLIEST_INUSE)) value = gReg[DMU(mmHUBPREQ3_DCSURF_PRIMARY_SURFACE_ADDRESS)];
        else if (reg == DMU(mmHUBPREQ3_DCSURF_SURFACE_EARLIEST_INUSE_HIGH)) value = gReg[DMU(mmHUBPREQ3_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH)];
        else if (reg == DMU(mmOTG0_OTG_MASTER_UPDATE_LOCK)) value = gReg[reg] | ((gReg[reg] & 1) ? 0x100 : 0);
        else value = gReg[reg];
        // CP_*_IC_OP_CNTL (GC1 + 0x5843, 47, 4B, 4F): the invalidation completes at once.
        if (reg == GC(mmCP_PFP_IC_OP_CNTL) || reg == GC(mmCP_ME_IC_OP_CNTL) || reg == GC(mmCP_CE_IC_OP_CNTL) || reg == GC(mmCP_CPC_IC_OP_CNTL)) value = (gReg[reg] & 1) ? (gReg[reg] | 2) : gReg[reg];
        if (gTrace) fprintf(gTrace, "R %05x %08x\n", reg, value);
        gFakeUs += 1;
        return true;
    }

    bool wr(uint32_t reg, uint32_t value)
    {
        if (forbidden(reg, true, value)) { ++gRefused; if (gTrace) fprintf(gTrace, "X %05x %08x\n", reg, value); return false; }
        ++gWrites;
        gReg[reg] = value;
        if (gTrace) fprintf(gTrace, "W %05x %08x\n", reg, value);
        // As seen on the console with the queue left active by Linux: the write pointer register does not take a value.
        if (reg == rCpRb0Wptr) gReg[reg] = 0;
        return true;
    }

    void unmap(void*) {}

    void* map(uint64_t phys, uint64_t bytes, bool)
    {
        if ((phys & 0xFFFF) || (bytes & 0xFFF) || phys < kCarvePhys || phys + bytes > kCarvePhys + kVramBytes) {
            printf("MOCK: map 0x%llx + 0x%llx refused\n", (unsigned long long)phys, (unsigned long long)bytes);
            return nullptr;
        }
        return gVram + (phys - kCarvePhys);
    }

    bool doorbell64(uint32_t byteOffset, uint64_t value)
    {
        uint32_t ctl = gReg[GC(mmCP_RB_DOORBELL_CONTROL)];
        bool en = ctl & CP_RB_DOORBELL_CONTROL__DOORBELL_EN_MASK;
        uint32_t off = (ctl & CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK) >> CP_RB_DOORBELL_CONTROL__DOORBELL_OFFSET__SHIFT;
        if (!en || off * 4 != byteOffset) { printf("MOCK: doorbell at 0x%x ignored (control 0x%08x)\n", byteOffset, ctl); ++gBadPackets; return true; }
        // Like the console: more than 4096 dwords outstanding and the CP loses count.
        if (value - gDoorbellSeen > 0x1000 + (uint64_t)((gRptr - (uint32_t)gDoorbellSeen) & kRingMask)) { printf("MOCK: more than 4096 dwords outstanding\n"); ++gBadPackets; }
        gDoorbellSeen = value;
        runRing((uint32_t)value & kRingMask);
        return true;
    }

    void delayUs(uint32_t us) { gFakeUs += us; }
    uint64_t nowUs() { return gFakeUs; }
    void flushWrites() {}
    uint32_t refusedCount() { return gRefused; }

    void report()
    {
        printf("MOCK: %u reads, %u writes, %u refused; ring: %u packets, %u fills, %u indirect buffers, %u TLB flushes in the ring, %u bad\n", gReads, gWrites, gRefused, gPackets, gFills, gIbs, gFlushes, gBadPackets);
    }
    bool clean() { return gBadPackets == 0 && gRefused == 0; }
    uint8_t* vram() { return gVram; }
}
