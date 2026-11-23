// SPDX-License-Identifier: MIT
// What stage 2 needs from the machine. Two implementations: backend_win.cpp (ps5gpu.sys) and backend_mock.cpp
// (a register and memory model with a tiny command-processor simulator, for testing on the Mac).
#pragma once
#include <stdint.h>

namespace be
{
    bool open();                                 // may be called more than once
    void lock();                                 // system-wide lock around the shared GPU state (lib/ps5drm.cpp)
    void unlock();
    bool lockReady();
    bool envFlag(const char* name);              // a process environment variable is set to something other than 0                            // the lock can be taken (checked by p5_open before anything else)
    uint32_t pid();
    bool pidAlive(uint32_t pid);
    bool isCompositor();                         // this process is the desktop compositor (dwm.exe): it may use the reserved client slots
    uint64_t bootStamp();                        // the same for every program until Windows restarts
    bool arm(bool on);
    bool rd(uint32_t reg, uint32_t& value);     // false: the driver refused the access
    bool wr(uint32_t reg, uint32_t value);
    void* map(uint64_t phys, uint64_t bytes, bool writeCombining);
    void unmap(void* va);                        // va: exactly what map returned
    void delayUs(uint32_t us);
    uint64_t nowUs();
    void flushWrites();                          // write-combining buffers out to memory
    bool doorbell64(uint32_t byteOffset, uint64_t value);   // one 64-bit store into the doorbell BAR (BAR2)
    uint32_t refusedCount();
#ifdef PS5GPU_MOCK
    uint32_t lastPid();                          // what pid() returned last
    void killPid(uint32_t pid);                  // pidAlive() says no for this one from now on
    void report();
    bool clean();
    uint8_t* vram();
#endif
}
