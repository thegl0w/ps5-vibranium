// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the ps5win contributors
//
// Microcode "direct load" for the AMD Cyan Skillfish GPU (GC 10.1.3, SDMA 5.0.1) in the PS5: the RLC, CP (PFP, CE,
// ME, MEC) and SDMA firmware is written over MMIO instead of being handed to the PSP. The register accesses follow
// Linux's amdgpu (gfx_v10_0.c, sdma_v5_0.c, MIT licensed) for firmware.load_type = DIRECT, plus the PS5's firmware
// wrapping (each payload sits inside a 0x100-byte header / 0x100-byte signature container).
//
// No OS dependency: all register access and delays go through the caller's Io callbacks. The only registers written
// are the ones named in the Reg namespace below. Nothing here talks to the PSP or the SMU or resets the bus.
#pragma once

#include <cstddef>
#include <cstdint>

namespace CyanDirectLoad {

// Register access supplied by the caller. `reg` is an absolute dword offset into the register BAR.
struct Io {
    void* ctx;
    uint32_t (*read)(void* ctx, uint32_t reg);
    void (*write)(void* ctx, uint32_t reg, uint32_t value);
    void (*delayUs)(void* ctx, uint32_t us);
};

// Absolute dword offsets and bit masks used by this component (IP bases as on Navi 10).
namespace Reg {
constexpr uint32_t kGc0 = 0x1260;
constexpr uint32_t kGc1 = 0xA000;

constexpr uint32_t GrbmStatus = kGc0 + 0x0DA4;
constexpr uint32_t GrbmSoftReset = kGc0 + 0x0DA8;
constexpr uint32_t CpMecCntl = kGc0 + 0x0E2D;
constexpr uint32_t CpStat = kGc0 + 0x0F40;
constexpr uint32_t CpMeCntl = kGc0 + 0x0F56;

constexpr uint32_t RlcCntl = kGc1 + 0x4C00;
constexpr uint32_t RlcGpmUcodeAddr = kGc1 + 0x5B61;
constexpr uint32_t RlcGpmUcodeData = kGc1 + 0x5B62;
constexpr uint32_t CpgPspDebug = kGc1 + 0x5C10;
constexpr uint32_t CpcPspDebug = kGc1 + 0x5C11;

// One CP microengine's instruction-cache and jump-table registers.
struct CpEngine {
    uint32_t icBaseLo, icBaseHi, icBaseCntl, icOpCntl, ucodeAddr, ucodeData;
};
constexpr CpEngine Pfp = { kGc1 + 0x5840, kGc1 + 0x5841, kGc1 + 0x5842, kGc1 + 0x5843, kGc1 + 0x5814, kGc1 + 0x5815 };
constexpr CpEngine Me = { kGc1 + 0x5844, kGc1 + 0x5845, kGc1 + 0x5846, kGc1 + 0x5847, kGc1 + 0x5816, kGc1 + 0x5817 };
constexpr CpEngine Ce = { kGc1 + 0x5848, kGc1 + 0x5849, kGc1 + 0x584A, kGc1 + 0x584B, kGc1 + 0x5818, kGc1 + 0x5819 };
constexpr CpEngine Mec = { kGc1 + 0x584C, kGc1 + 0x584D, kGc1 + 0x584E, kGc1 + 0x584F, kGc1 + 0x581A, kGc1 + 0x581B };

// SDMA instance 0; instance 1 is +kSdmaStride (engine registers) or +kSdmaUcodeStride (ucode pair).
constexpr uint32_t Sdma0Cntl = kGc0 + 0x001C;
constexpr uint32_t Sdma0Status = kGc0 + 0x0025;
constexpr uint32_t Sdma0F32Cntl = kGc0 + 0x002A;
constexpr uint32_t Sdma0GfxRbCntl = kGc0 + 0x0080;
constexpr uint32_t Sdma0GfxIbCntl = kGc0 + 0x008A;
constexpr uint32_t Sdma0UcodeAddr = kGc1 + 0x5880;
constexpr uint32_t Sdma0UcodeData = kGc1 + 0x5881;
constexpr uint32_t kSdmaStride = 0x600;
constexpr uint32_t kSdmaUcodeStride = 0x20;

// Bits.
constexpr uint32_t kSoftResetSdma0 = 0x00800000;
constexpr uint32_t kSoftResetSdma1 = 0x01000000;
constexpr uint32_t kMecMe1Halt = 0x40000000;
constexpr uint32_t kMecMe2Halt = 0x10000000;
constexpr uint32_t kMeCeHalt = 0x01000000;
constexpr uint32_t kMePfpHalt = 0x04000000;
constexpr uint32_t kMeMeHalt = 0x10000000;
constexpr uint32_t kGpaOverride = 0x8;
constexpr uint32_t kRlcUcodeStart = 0x2000;
constexpr uint32_t kIcInvalidate = 0x1;
constexpr uint32_t kIcInvalidateDone = 0x2;
constexpr uint32_t kIcVmid = 0x0000000F;
constexpr uint32_t kIcAddressClamp = 0x00000010;
constexpr uint32_t kIcExeDisable = 0x00800000;
constexpr uint32_t kIcCachePolicy = 0x03000000;
constexpr uint32_t kSdmaHalt = 0x1;
constexpr uint32_t kSdmaAutoCtxsw = 0x00040000;
constexpr uint32_t kSdmaRingEnable = 0x1;
} // namespace Reg

enum class Kind : uint8_t { Rlc, Sdma, CpGfx, CpMec };

// What parse() extracts from a firmware file. Pointers point into the caller's file buffer.
struct Image {
    const uint8_t* code;   // code to load (inside the PSP container)
    uint32_t codeBytes;
    const uint8_t* jt;     // jump table (CP only, else null)
    uint32_t jtDwords;
    uint32_t jtDwordsHdr;  // jt_size as stated in the file header (CP only, else 0)
    uint32_t version;      // ucode_version
};

// Checks a zstd-decompressed linux-firmware cyan_skillfish2_*.bin file and fills `out`. False if anything is off
// (`out` is then all zero).
bool parse(const uint8_t* file, size_t size, Kind kind, Image& out);

struct CpImages {
    Image pfp, ce, me, mec;
};

// GPU-visible memory the CP fetches its code from: `cpu` is the CPU mapping of GPU address `gpu`.
struct Store {
    uint8_t* cpu;
    uint64_t gpu;
    uint64_t bytes;
};

constexpr uint32_t kStoreAlign = 0x1000;

// PFP, CE, ME, MEC back to back, each rounded up to kStoreAlign.
uint64_t storeBytesNeeded(const CpImages& images);

enum class Status : uint8_t { Ok, BadImage, StoreTooSmall, StoreMismatch, IcacheTimeout };
const char* statusName(Status status);

struct Snapshot {
    uint32_t rlcCntl, rlcGpmUcodeAddr, grbmStatus;
    uint32_t cpMeCntl, cpMecCntl, cpStat;
    uint32_t cpgPspDebug, cpcPspDebug;
    uint32_t pfpIcBaseLo, pfpIcBaseHi, pfpIcBaseCntl;
    uint32_t ceIcBaseLo, ceIcBaseHi, ceIcBaseCntl;
    uint32_t meIcBaseLo, meIcBaseHi, meIcBaseCntl;
    uint32_t cpcIcBaseLo, cpcIcBaseHi, cpcIcBaseCntl;
    uint32_t sdmaF32Cntl[2], sdmaCntl[2], sdmaStatus[2], sdmaRbCntl[2];
};

// Reads only.
void snapshot(const Io& io, Snapshot& out);

struct Report {
    Status status;
    uint32_t step;            // step that failed (CP: 1 PFP, 2 CE, 3 ME, 4 MEC); 0 on success or when no step started
    uint32_t cpStatAfterHalt; // last CP_STAT read while waiting for the CP to halt
    uint32_t haltWaitUs;      // polling rounds (1 us each) before CP_STAT read 0; kHaltTimeoutUs if it never did
    uint32_t icacheWaitUs[4]; // same for each invalidate: PFP, CE, ME, MEC
    uint32_t dataWrites;      // dwords written to UCODE_DATA registers
};

constexpr uint32_t kHaltTimeoutUs = 100000;
constexpr uint32_t kIcacheTimeoutUs = 50000;

// Halts CP gfx, CP compute and both SDMA engines; optionally pulses the SDMA soft resets. Leaves the RLC alone.
void quiesce(const Io& io, bool resetSdma);
// Sets GPA_OVERRIDE in CPC_PSP_DEBUG and CPG_PSP_DEBUG.
void disableGpaMode(const Io& io);
// Writes the RLC microcode. The caller has stopped the RLC beforehand and starts it afterwards.
Report loadRlc(const Io& io, const Image& rlc);
// Copies the CP code into the store and checks it; no register access.
Status stageCp(const CpImages& images, const Store& store);
// Halts the CP, points each microengine's instruction cache at its code in the store and writes the jump tables.
// Everything stays halted.
Report loadCp(const Io& io, const CpImages& images, const Store& store);
// True if the store still holds exactly the CP code.
bool storeIntact(const CpImages& images, const Store& store);
// Halts both SDMA engines and writes their microcode. They stay halted.
Report loadSdma(const Io& io, const Image& sdma0, const Image& sdma1);

} // namespace CyanDirectLoad
