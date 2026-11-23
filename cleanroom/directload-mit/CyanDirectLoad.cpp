// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the ps5win contributors
//
// See CyanDirectLoad.hpp. Register sequences after Linux's amdgpu gfx_v10_0.c / sdma_v5_0.c (MIT), firmware layout
// after amdgpu_ucode.h plus the PS5's PSP container wrapping.
#include "CyanDirectLoad.hpp"

#include <cstring>

namespace CyanDirectLoad {
namespace {

// ---- firmware file layout ----

// amdgpu common firmware header (amdgpu_ucode.h, struct common_firmware_header) and the gfx extension.
constexpr size_t kHdrSizeBytes = 0x00;
constexpr size_t kHdrUcodeVersion = 0x10;
constexpr size_t kHdrUcodeSize = 0x14;
constexpr size_t kHdrUcodeOffset = 0x18;
constexpr size_t kHdrJtOffset = 0x24;  // dwords
constexpr size_t kHdrJtSize = 0x28;    // dwords
constexpr size_t kMinFileBytes = 0x2C;

// PS5 PSP container around each payload: header, inner code, signature.
constexpr uint32_t kBoxHeader = 0x100;
constexpr uint32_t kBoxSignature = 0x100;
constexpr uint32_t kBoxOverhead = kBoxHeader + kBoxSignature;
constexpr size_t kBoxMagicAt = 0x10;
constexpr size_t kBoxInnerSizeAt = 0x14;
constexpr uint32_t kBoxMagic = 0x31535024; // "$PS1"

uint32_t le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A PSP container header at `box` (at least kBoxHeader bytes available). Returns false if the magic is wrong.
bool boxInnerSize(const uint8_t* box, uint32_t& inner)
{
    if (le32(box + kBoxMagicAt) != kBoxMagic) return false;
    inner = le32(box + kBoxInnerSizeAt);
    return true;
}

// ---- store layout ----

uint64_t roundUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

struct Slot {
    const Image* image;
    uint64_t offset;
};

void storeSlots(const CpImages& images, Slot (&slots)[4])
{
    const Image* order[4] = { &images.pfp, &images.ce, &images.me, &images.mec };
    uint64_t off = 0;
    for (int i = 0; i < 4; ++i) {
        slots[i] = { order[i], off };
        off += roundUp(order[i]->codeBytes, kStoreAlign);
    }
}

bool imageUsable(const Image& im, bool needJt)
{
    if (!im.code || im.codeBytes == 0 || (im.codeBytes & 3)) return false;
    if (needJt && (!im.jt || im.jtDwords == 0)) return false; // a CP engine needs a non-empty jump table
    return true;
}

bool cpImagesUsable(const CpImages& images)
{
    return imageUsable(images.pfp, true) && imageUsable(images.ce, true) && imageUsable(images.me, true) &&
           imageUsable(images.mec, true);
}

Status checkStore(const CpImages& images, const Store& store)
{
    if (!cpImagesUsable(images)) return Status::BadImage;
    // One status for "too small or not usable": null mapping, GPU address not page aligned, or not enough bytes.
    if (!store.cpu || (store.gpu & (kStoreAlign - 1)) || store.bytes < storeBytesNeeded(images)) return Status::StoreTooSmall;
    return Status::Ok;
}

bool copyAndCheck(const Slot& s, const Store& store)
{
    std::memcpy(store.cpu + s.offset, s.image->code, s.image->codeBytes);
    return std::memcmp(store.cpu + s.offset, s.image->code, s.image->codeBytes) == 0;
}

// ---- register helpers ----

uint32_t rd(const Io& io, uint32_t reg) { return io.read(io.ctx, reg); }
void wr(const Io& io, uint32_t reg, uint32_t v) { io.write(io.ctx, reg, v); }
void delay(const Io& io, uint32_t us) { io.delayUs(io.ctx, us); }
void setBits(const Io& io, uint32_t reg, uint32_t bits) { wr(io, reg, rd(io, reg) | bits); }
void clearBits(const Io& io, uint32_t reg, uint32_t bits) { wr(io, reg, rd(io, reg) & ~bits); }

// Up to timeoutUs rounds of: read `reg`, stop if (value & mask) == want, sleep 1 us. `waitedUs` is the number of
// rounds that did not see the value (= timeoutUs when it never came).
bool poll(const Io& io, uint32_t reg, uint32_t mask, uint32_t want, uint32_t timeoutUs, uint32_t& waitedUs, uint32_t& last)
{
    for (waitedUs = 0; waitedUs < timeoutUs; ++waitedUs) {
        last = rd(io, reg);
        if ((last & mask) == want) return true;
        delay(io, 1);
    }
    return false;
}

void writeDwords(const Io& io, uint32_t dataReg, const uint8_t* src, uint32_t dwords, uint32_t& counter)
{
    for (uint32_t i = 0; i < dwords; ++i) wr(io, dataReg, le32(src + 4 * i));
    counter += dwords;
}

// gfx_v10_0_cp_gfx_enable(false): halt PFP, CE, ME and wait for CP_STAT to go idle.
void haltCpGfx(const Io& io, uint32_t& cpStat, uint32_t& waitedUs)
{
    setBits(io, Reg::CpMeCntl, Reg::kMeCeHalt | Reg::kMePfpHalt | Reg::kMeMeHalt);
    poll(io, Reg::CpStat, 0xFFFFFFFF, 0, kHaltTimeoutUs, waitedUs, cpStat);
}

// gfx_v10_0_cp_compute_enable(false).
void haltCpCompute(const Io& io)
{
    wr(io, Reg::CpMecCntl, Reg::kMecMe1Halt | Reg::kMecMe2Halt);
    delay(io, 50);
}

void sdmaCtxSwitchOff(const Io& io)
{
    for (uint32_t i = 0; i < 2; ++i) clearBits(io, Reg::Sdma0Cntl + i * Reg::kSdmaStride, Reg::kSdmaAutoCtxsw);
}

// sdma_v5_0_gfx_stop + sdma_v5_0_enable(false).
void sdmaStopAndHalt(const Io& io)
{
    for (uint32_t i = 0; i < 2; ++i) {
        clearBits(io, Reg::Sdma0GfxRbCntl + i * Reg::kSdmaStride, Reg::kSdmaRingEnable);
        clearBits(io, Reg::Sdma0GfxIbCntl + i * Reg::kSdmaStride, Reg::kSdmaRingEnable);
    }
    for (uint32_t i = 0; i < 2; ++i) setBits(io, Reg::Sdma0F32Cntl + i * Reg::kSdmaStride, Reg::kSdmaHalt);
}

// One CP microengine: code into the store, instruction cache invalidated and pointed at it, jump table written.
Status loadCpEngine(const Io& io, const Reg::CpEngine& e, const Slot& s, const Store& store, bool writeBaseCntl,
                    bool keepVmid, uint32_t& icacheWaitUs, uint32_t& dataWrites)
{
    if (!copyAndCheck(s, store)) return Status::StoreMismatch;

    setBits(io, e.icOpCntl, Reg::kIcInvalidate);
    uint32_t last = 0;
    if (!poll(io, e.icOpCntl, Reg::kIcInvalidateDone, Reg::kIcInvalidateDone, kIcacheTimeoutUs, icacheWaitUs, last))
        return Status::IcacheTimeout;

    uint32_t cntl = rd(io, e.icBaseCntl);
    if (!keepVmid) cntl &= ~Reg::kIcVmid;
    cntl &= ~(Reg::kIcCachePolicy | Reg::kIcExeDisable);
    cntl |= Reg::kIcAddressClamp;
    if (writeBaseCntl) wr(io, e.icBaseCntl, cntl);

    uint64_t addr = store.gpu + s.offset;
    wr(io, e.icBaseLo, (uint32_t)addr & 0xFFFFF000u);
    wr(io, e.icBaseHi, (uint32_t)(addr >> 32));

    wr(io, e.ucodeAddr, 0);
    writeDwords(io, e.ucodeData, s.image->jt, s.image->jtDwords, dataWrites);
    wr(io, e.ucodeAddr, s.image->version);
    return Status::Ok;
}

Report freshReport()
{
    Report r;
    std::memset(&r, 0, sizeof r);
    r.status = Status::Ok;
    return r;
}

} // namespace

// ---- parsing ----

bool parse(const uint8_t* file, size_t size, Kind kind, Image& out)
{
    out = Image{};
    if (!file || size < kMinFileBytes) return false;
    if (le32(file + kHdrSizeBytes) != size) return false;

    const uint32_t version = le32(file + kHdrUcodeVersion);
    const uint32_t payloadBytes = le32(file + kHdrUcodeSize);
    const uint32_t payloadAt = le32(file + kHdrUcodeOffset);
    if ((uint64_t)payloadAt + payloadBytes > size) return false;
    if ((payloadBytes & 3) || payloadBytes <= kBoxOverhead) return false;

    const uint8_t* payload = file + payloadAt;
    const uint32_t codeBytes = payloadBytes - kBoxOverhead;
    uint32_t inner = 0;
    if (!boxInnerSize(payload, inner)) return false;

    Image im = {};
    im.code = payload + kBoxHeader;
    im.codeBytes = codeBytes;
    im.version = version;

    switch (kind) {
    case Kind::Rlc:
    case Kind::Sdma:
        if (inner != codeBytes) return false;
        break;
    case Kind::CpGfx: {
        if (inner != codeBytes) return false;
        const uint64_t jtAt = (uint64_t)le32(file + kHdrJtOffset) * 4;
        const uint64_t jtDw = le32(file + kHdrJtSize);
        if (jtDw == 0 || jtAt + jtDw * 4 > codeBytes) return false;
        im.jt = im.code + jtAt;
        im.jtDwords = (uint32_t)jtDw;
        im.jtDwordsHdr = (uint32_t)jtDw;
        break;
    }
    case Kind::CpMec: {
        // [hdr][MEC code: inner][sig] [hdr][jump table: inner2][sig]
        const uint64_t second = (uint64_t)kBoxOverhead + inner;
        if (second + kBoxOverhead > payloadBytes) return false;
        if (second != (uint64_t)le32(file + kHdrJtOffset) * 4) return false;
        uint32_t inner2 = 0;
        if (!boxInnerSize(payload + second, inner2)) return false;
        if (second + kBoxOverhead + inner2 != payloadBytes) return false;
        if (inner2 == 0 || (inner2 & 3)) return false;
        im.jt = payload + second + kBoxHeader;
        im.jtDwords = inner2 / 4;
        im.jtDwordsHdr = le32(file + kHdrJtSize);
        break;
    }
    default:
        return false;
    }
    out = im;
    return true;
}

// ---- small things ----

uint64_t storeBytesNeeded(const CpImages& images)
{
    Slot slots[4];
    storeSlots(images, slots);
    return slots[3].offset + roundUp(images.mec.codeBytes, kStoreAlign);
}

const char* statusName(Status status)
{
    switch (status) {
    case Status::Ok: return "Ok";
    case Status::BadImage: return "BadImage";
    case Status::StoreTooSmall: return "StoreTooSmall";
    case Status::StoreMismatch: return "StoreMismatch";
    case Status::IcacheTimeout: return "IcacheTimeout";
    }
    return "?";
}

void snapshot(const Io& io, Snapshot& s)
{
    s.rlcCntl = rd(io, Reg::RlcCntl);
    s.rlcGpmUcodeAddr = rd(io, Reg::RlcGpmUcodeAddr);
    s.grbmStatus = rd(io, Reg::GrbmStatus);
    s.cpMeCntl = rd(io, Reg::CpMeCntl);
    s.cpMecCntl = rd(io, Reg::CpMecCntl);
    s.cpStat = rd(io, Reg::CpStat);
    s.cpgPspDebug = rd(io, Reg::CpgPspDebug);
    s.cpcPspDebug = rd(io, Reg::CpcPspDebug);
    s.pfpIcBaseLo = rd(io, Reg::Pfp.icBaseLo);
    s.pfpIcBaseHi = rd(io, Reg::Pfp.icBaseHi);
    s.pfpIcBaseCntl = rd(io, Reg::Pfp.icBaseCntl);
    s.ceIcBaseLo = rd(io, Reg::Ce.icBaseLo);
    s.ceIcBaseHi = rd(io, Reg::Ce.icBaseHi);
    s.ceIcBaseCntl = rd(io, Reg::Ce.icBaseCntl);
    s.meIcBaseLo = rd(io, Reg::Me.icBaseLo);
    s.meIcBaseHi = rd(io, Reg::Me.icBaseHi);
    s.meIcBaseCntl = rd(io, Reg::Me.icBaseCntl);
    s.cpcIcBaseLo = rd(io, Reg::Mec.icBaseLo);
    s.cpcIcBaseHi = rd(io, Reg::Mec.icBaseHi);
    s.cpcIcBaseCntl = rd(io, Reg::Mec.icBaseCntl);
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t d = i * Reg::kSdmaStride;
        s.sdmaF32Cntl[i] = rd(io, Reg::Sdma0F32Cntl + d);
        s.sdmaCntl[i] = rd(io, Reg::Sdma0Cntl + d);
        s.sdmaStatus[i] = rd(io, Reg::Sdma0Status + d);
        s.sdmaRbCntl[i] = rd(io, Reg::Sdma0GfxRbCntl + d);
    }
}

// ---- sequences ----

void quiesce(const Io& io, bool resetSdma)
{
    uint32_t stat = 0, waited = 0;
    haltCpGfx(io, stat, waited);
    haltCpCompute(io);
    sdmaCtxSwitchOff(io);
    sdmaStopAndHalt(io);
    if (!resetSdma) return;
    // sdma_v5_0_soft_reset: one engine at a time, 50 us pulse.
    const uint32_t bits[2] = { Reg::kSoftResetSdma0, Reg::kSoftResetSdma1 };
    for (uint32_t i = 0; i < 2; ++i) {
        uint32_t v = rd(io, Reg::GrbmSoftReset) | bits[i];
        wr(io, Reg::GrbmSoftReset, v);
        v = rd(io, Reg::GrbmSoftReset);
        delay(io, 50);
        wr(io, Reg::GrbmSoftReset, v & ~bits[i]);
        (void)rd(io, Reg::GrbmSoftReset);
    }
    sdmaStopAndHalt(io);
}

void disableGpaMode(const Io& io)
{
    setBits(io, Reg::CpcPspDebug, Reg::kGpaOverride);
    setBits(io, Reg::CpgPspDebug, Reg::kGpaOverride);
}

Report loadRlc(const Io& io, const Image& rlc)
{
    Report r = freshReport();
    if (!imageUsable(rlc, false)) {
        r.status = Status::BadImage; // step stays 0: no step started
        return r;
    }
    wr(io, Reg::RlcGpmUcodeAddr, Reg::kRlcUcodeStart);
    writeDwords(io, Reg::RlcGpmUcodeData, rlc.code, rlc.codeBytes / 4, r.dataWrites);
    wr(io, Reg::RlcGpmUcodeAddr, rlc.version);
    return r;
}

Status stageCp(const CpImages& images, const Store& store)
{
    Status st = checkStore(images, store);
    if (st != Status::Ok) return st;
    Slot slots[4];
    storeSlots(images, slots);
    for (const Slot& s : slots)
        if (!copyAndCheck(s, store)) return Status::StoreMismatch;
    return Status::Ok;
}

bool storeIntact(const CpImages& images, const Store& store)
{
    if (checkStore(images, store) != Status::Ok) return false;
    Slot slots[4];
    storeSlots(images, slots);
    for (const Slot& s : slots)
        if (std::memcmp(store.cpu + s.offset, s.image->code, s.image->codeBytes) != 0) return false;
    return true;
}

Report loadCp(const Io& io, const CpImages& images, const Store& store)
{
    Report r = freshReport();
    r.status = checkStore(images, store);
    if (r.status != Status::Ok) return r;

    Slot slots[4];
    storeSlots(images, slots);

    haltCpGfx(io, r.cpStatAfterHalt, r.haltWaitUs);
    // gfx_v10_0_cp_gfx_load_{pfp,ce,me}_microcode. Only PFP gets its IC_BASE_CNTL written back.
    const Reg::CpEngine* gfx[3] = { &Reg::Pfp, &Reg::Ce, &Reg::Me };
    for (uint32_t i = 0; i < 3; ++i) {
        r.status = loadCpEngine(io, *gfx[i], slots[i], store, i == 0, false, r.icacheWaitUs[i], r.dataWrites);
        if (r.status != Status::Ok) {
            r.step = i + 1;
            return r;
        }
    }
    // gfx_v10_0_cp_compute_load_microcode: VMID kept, IC_BASE_CNTL written.
    haltCpCompute(io);
    r.status = loadCpEngine(io, Reg::Mec, slots[3], store, true, true, r.icacheWaitUs[3], r.dataWrites);
    if (r.status != Status::Ok) r.step = 4;
    return r;
}

Report loadSdma(const Io& io, const Image& sdma0, const Image& sdma1)
{
    Report r = freshReport();
    const Image* im[2] = { &sdma0, &sdma1 };
    for (uint32_t i = 0; i < 2; ++i) {
        if (!imageUsable(*im[i], false)) {
            r.status = Status::BadImage; // step stays 0: no step started
            return r;
        }
    }
    sdmaStopAndHalt(io);
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t d = i * Reg::kSdmaUcodeStride;
        wr(io, Reg::Sdma0UcodeAddr + d, 0);
        writeDwords(io, Reg::Sdma0UcodeData + d, im[i]->code, im[i]->codeBytes / 4, r.dataWrites);
        wr(io, Reg::Sdma0UcodeAddr + d, im[i]->version);
    }
    return r;
}

} // namespace CyanDirectLoad
