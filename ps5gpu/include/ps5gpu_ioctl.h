// SPDX-License-Identifier: MIT
// ps5gpu.sys: interface between the kernel shim and user mode.
//
// The kernel part is deliberately small. It owns the GPU's register BAR and is the only code that touches it, so
// the PSP/SMU firewall cannot be bypassed. Everything else (microcode load, rings, page tables, command submission)
// runs in user mode on top of these calls: a mistake there ends a process, not Windows.
#pragma once
#include <stdint.h>

#define PS5GPU_VERSION      0x00020000u     // 2: PS5GPU_IOCTL_CORE (several client programs at a time)
#define PS5GPU_WIN32_PATH   "\\\\.\\Ps5Gpu"

#define PS5GPU_CTL(n)       ((0x8000u << 16) | (0u << 14) | ((0x800u + (n)) << 2) | 0u)   // METHOD_BUFFERED, FILE_ANY_ACCESS
#define PS5GPU_IOCTL_INFO   PS5GPU_CTL(0)   // out: PS5GPU_INFO
#define PS5GPU_IOCTL_REGS   PS5GPU_CTL(1)   // in/out: PS5GPU_REGOP[]
#define PS5GPU_IOCTL_MAP    PS5GPU_CTL(2)   // in/out: PS5GPU_MAP
#define PS5GPU_IOCTL_UNMAP  PS5GPU_CTL(3)   // in: PS5GPU_MAP (UserVa)
#define PS5GPU_IOCTL_ARM    PS5GPU_CTL(4)   // in: uint32_t magic. Until then every register write is refused.
#define PS5GPU_IOCTL_LOG    PS5GPU_CTL(5)   // out: text, the driver's log
#define PS5GPU_IOCTL_CORE   PS5GPU_CTL(6)   // in/out: P5CORE_CALL (see below)

#define PS5GPU_ARM_MAGIC    0x57473550u     // "P5GW": allow register writes
#define PS5GPU_DISARM_MAGIC 0x52473550u     // "P5GR": back to read-only

// Info.Flags
#define PS5GPU_F_FOUND      0x1u            // the expected GPU is there and its register BAR is mapped
#define PS5GPU_F_ARMED      0x2u
#define PS5GPU_F_VM         0x4u            // rehearsal build: the "GPU" is QEMU's VGA device

typedef struct PS5GPU_INFO {
    uint32_t Version;
    uint32_t Flags;
    uint32_t Bus, Device, Function;
    uint32_t VendorDevice;                  // config dword 0x00
    uint32_t CommandStatus;                 // config dword 0x04
    uint32_t ClassRev;                      // config dword 0x08
    uint32_t Subsystem;                     // config dword 0x2C
    uint32_t RegBytes;                      // size of the register BAR
    uint64_t Bar[6];                        // decoded bases (64-bit BARs joined; the upper half's slot is 0)
    uint64_t FbPhys, FbBytes;               // range user mode may map besides the BARs (set by the first MAP that passes the checks)
    uint32_t Refused;                       // register operations refused by the firewall since load
    uint32_t RefusedLastReg, RefusedLastValue;
    uint32_t Reads, Writes;                 // register accesses made
    uint32_t Maps;                          // live user mappings
    uint32_t Clients;                       // programs attached through PS5GPU_IOCTL_CORE right now
    uint32_t Reserved[7];
} PS5GPU_INFO;

// Register operations. Reg is a dword index into the register BAR, as Linux's RREG32/WREG32 take it.
#define PS5GPU_OP_READ      0u              // Value <- register
#define PS5GPU_OP_WRITE     1u              // register <- Value; if Mask is not 0: register <- (old & ~Mask) | (Value & Mask)
#define PS5GPU_OP_DELAY     2u              // wait Value microseconds (at most 100000)
#define PS5GPU_OP_WAIT      3u              // poll until (register & Mask) == Value, at most Aux microseconds; Value <- last read
#define PS5GPU_OP_STREAM    4u              // write the next Aux operations' Value fields to Reg (their Op must be PS5GPU_OP_DATA)
#define PS5GPU_OP_DATA      5u              // payload of a STREAM
#define PS5GPU_OP_MASK      0xFFu
// Set by the driver in Op on return:
#define PS5GPU_OP_DONE      0x10000000u
#define PS5GPU_OP_TIMEOUT   0x20000000u     // WAIT ran out; later operations were not run
#define PS5GPU_OP_REFUSED   0x40000000u     // firewall, not armed, or malformed; later operations were not run

typedef struct PS5GPU_REGOP {
    uint32_t Op, Reg, Value, Mask, Aux;
} PS5GPU_REGOP;

// Map physical memory into the calling process. Allowed: inside BAR0 (VRAM window) or BAR2 (doorbells), or a range
// above 4 GiB that Windows does not use as RAM (the VRAM carve-out). Never the register BAR.
#define PS5GPU_MAP_WC       0x1u            // write-combining (VRAM); otherwise uncached (doorbells)
typedef struct PS5GPU_MAP {
    uint64_t Phys, Bytes;
    uint64_t UserVa;                        // out for MAP, in for UNMAP
    uint32_t Flags, Reserved;
} PS5GPU_MAP;

// ---- several client programs at a time (PS5GPU_IOCTL_CORE) ----
//
// The parts of a graphics kernel driver that must be shared between programs live in the kernel driver (core/p5core.h):
// who owns which piece of VRAM, one GPU address space per program (VMID 1-15, page tables in the pool), and the one
// command ring all submissions go through. A program attaches with OPEN on its own handle to the device; closing the
// handle (or the program ending) gives everything back. Buffer memory itself is still mapped into the program with
// PS5GPU_IOCTL_MAP, and "has submission N finished" is still read straight from the fence page.
//
// One structure for every operation; Result is 0 or a negative errno-style value. Only SUBMIT uses Count and Ib.
#define P5CORE_OP_OPEN       1u   // out A0 carve-out physical, A1 its GPU (MC) address, A2 VRAM bytes, A3 area offset, A4 heap end offset,
                                  //     A5 vmid | client count << 32, A6 active render-backend mask | active CU count << 32, A7 not used yet
#define P5CORE_OP_CLOSE      2u
#define P5CORE_OP_INFO       3u   // out A0 heap bytes, A1 heap free, A2 this client's last sequence number, A3 its buffers, A4 its page-table pages, A5 clients
#define P5CORE_OP_BO_ALLOC   4u   // in A0 bytes, A1 alignment; out A0 handle, A1 VRAM offset, A2 bytes (rounded up to pages)
#define P5CORE_OP_BO_FREE    5u   // in A0 handle
#define P5CORE_OP_VA         6u   // in A0 handle, A1 offset in the buffer, A2 bytes, A3 GPU virtual address, A4 flags, A5 operation (P5_VA_*)
#define P5CORE_OP_VA_QUERY   7u   // in A0 GPU virtual address; out A0 physical address of that byte (0: nothing mapped)
#define P5CORE_OP_CTX_CREATE 8u   // out A0 context id
#define P5CORE_OP_CTX_FREE   9u   // in A0 context id
#define P5CORE_OP_SUBMIT     10u  // in A0 context, A1 fence buffer handle (0: none), A2 offset in it, A3 flags (P5CORE_SUBMIT_*), Count, Ib[]; out A0 sequence number
#define P5CORE_SUBMIT_FENCE_IRQ 0x1u   // also raise the end-of-pipe interrupt when done

#define P5CORE_MAX_IBS       32u
typedef struct P5CORE_IB {
    uint64_t Va;
    uint32_t Bytes, Flags;                  // Flags: P5_IB_* of lib/ps5drm.h
} P5CORE_IB;
typedef struct P5CORE_CALL {
    uint32_t Op;
    int32_t Result;
    uint64_t A[8];
    uint32_t Count, Reserved;
    P5CORE_IB Ib[P5CORE_MAX_IBS];
} P5CORE_CALL;
#define P5CORE_CALL_HEAD     80u            // bytes before Ib; every operation except SUBMIT may pass just these
