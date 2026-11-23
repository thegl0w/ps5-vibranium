// SPDX-License-Identifier: MIT
// Where things are in VRAM, shared by the bring-up tool (stage2.cpp) and the library (lib/ps5drm.cpp).
//
//   0 .. 0x20000000                 not ours: the scan-out image lives somewhere in here
//   0x20000000 .. area              buffer heap (the library)
//   area = top - 512 MiB, 16 MiB    ring, pointers, fence, shared state, clear-state buffer, CP microcode, step-8 test
//   area + 16 MiB, 64 MiB           page-table pool for VMID 1 (the library)
//   top megabyte                    Linux's old GART table (emptied in step 2)
#pragma once
#include <stdint.h>

namespace p5layout
{
    constexpr uint64_t kAreaBelowTop = 0x20000000ull, kAreaBytes = 0x1000000ull;
    // Ring size as Linux uses it for this queue: 2048 dwords (CP_GFX_HQD_CNTL 0x00A0080A on the console). With a
    // 16384-dword ring the CP lost track of outstanding data whenever more than 4096 dwords were waiting: it then ran
    // a fixed 0x2000-0x3000 dwords behind the write pointer for good (2026-11-19, libtest: 47-48 submissions never
    // executed; each further doorbell advanced it by exactly the new amount).
    constexpr uint64_t kRingOff = 0x000000, kRingBytes = 0x2000;      // 2048 dwords
    constexpr uint64_t kRptrOff = 0x010000, kWptrOff = 0x011000, kDummyOff = 0x012000, kFenceOff = 0x013000, kSharedOff = 0x014000;
    constexpr uint64_t kCsbOff = 0x020000;
    constexpr uint64_t kStoreOff = 0x100000, kStoreBytes = 0x200000;
    constexpr uint64_t kPtOff = 0x400000;                              // step 8: four page-table pages
    constexpr uint64_t kUserOff = 0x800000, kUserBytes = 0x200000;     // step 8: memory given to VMID 1
    constexpr uint32_t kDoorbellIndex = 0x8B * 2;                      // AMDGPU_NAVI10_DOORBELL_GFX_RING0 << 1
    constexpr uint64_t kHeapStart = 0x20000000ull;
    constexpr uint64_t kPtPoolBytes = 0x4000000ull;                    // right after the area

    // Ring state that outlives a process, at area + kSharedOff. The fence value (last finished sequence number, 64-bit)
    // is at area + kFenceOff and is written by the GPU.
    struct Shared {
        uint32_t magic, version;
        uint64_t wptr;      // dwords written to the ring since it was started; never wraps
        uint64_t seq;       // last sequence number handed out
    };
    constexpr uint32_t kSharedMagic = 0x53473550;                      // "P5GS"
}
