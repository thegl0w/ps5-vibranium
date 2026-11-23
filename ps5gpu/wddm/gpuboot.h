// SPDX-License-Identifier: MIT
// GPU start without an operating system underneath: the same steps as `ps5gpu2 run` (tool/stage2.cpp, steps 1-6 and 8),
// written against a small table of callbacks so that one source runs in two places:
//   - inside ps5wddm.sys at boot (DriverEntry, before the driver decides between display-only and full mode),
//   - on the Mac against the register/memory model (tool/backend_mock.cpp), where its register writes are compared
//     with stage2's (build-gpuboot-mock.sh).
// No allocation, no C library beyond memcpy/memset/memcmp, no floating point, no exceptions.
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

namespace gpuboot
{
    struct Env {
        void* ctx;
        bool (*rd)(void* ctx, uint32_t reg, uint32_t* value);           // false: refused (never happens for these steps)
        bool (*wr)(void* ctx, uint32_t reg, uint32_t value);
        void (*delayUs)(void* ctx, uint32_t us);
        uint64_t (*nowUs)(void* ctx);
        uint8_t* (*mapVram)(void* ctx, uint64_t phys, uint64_t bytes);  // write-combined CPU view of carve-out memory, or null
        void (*unmapVram)(void* ctx, uint8_t* va, uint64_t bytes);
        bool (*doorbell64)(void* ctx, uint32_t byteOffset, uint64_t value);   // one 64-bit store into BAR2
        void (*flushWrites)(void* ctx);                                 // write-combining buffers out (sfence)
        void (*line)(void* ctx, const char* text);                      // one log line
        void (*step)(void* ctx, uint32_t step, const char* name);       // about to start this step (the caller persists it)
        int (*vformat)(char* out, size_t bytes, const char* fmt, va_list ap);
    };

    // The seven microcode files, in this order (linux-firmware names; the same files ps5gpu2 reads from C:\ps5\gpu\fw).
    constexpr int kFwCount = 7;
    extern const char* const kFwNames[kFwCount];
    struct Files {
        const uint8_t* data[kFwCount];
        size_t bytes[kFwCount];
    };

    struct Options {
        bool drawTest;           // step 7 (a rectangle on the screen): off at boot; on only to compare with stage2 on the mock
        bool refuseIfClients;    // stop if libps5gpu's state says programs are attached (a live restart; not at boot)
        uint32_t budgetMs;       // the whole start must finish within this (checked between steps); 0 = no limit
    };

    enum Result : uint32_t {
        kOk = 0,
        kBadFirmware = 1,        // a file missing or not as expected
        kLayout = 2,             // GPU memory layout not as expected (the same checks as stage2's discover)
        kNoArea = 3,             // our 16 MiB area cannot be mapped or does not hold data
        kClients = 4,            // programs are using the GPU
        kRefused = 5,            // a register access was refused
        kGart = 6,               // step 2
        kRlc = 7,                // step 3
        kCp = 8,                 // step 4
        kRing = 9,               // step 5
        kRingTest = 10,          // step 6
        kFill = 11,              // step 7 (test only)
        kVm = 12,                // step 8
        kTimeout = 13,           // budget used up
    };

    // Runs the start. On failure `reason` says why in one line. The GPU is then left part-started; ps5gpu2 run (the
    // autostart path) starts it from any state.
    Result run(const Env& env, const Files& files, const Options& opt, char* reason, size_t reasonBytes);
}
