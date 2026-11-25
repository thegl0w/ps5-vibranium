// SPDX-License-Identifier: MIT
//
// Warm restart for Windows on the PS5: Restart re-enters our stage 1 without a platform reset (design and experiment
// results: windows-handoff/WARM-RESTART-STUDY.md). Linked into Ps5IccResetSystemLib (ResetSystemRuntimeDxe).
//
// Two pieces of memory carry it:
//  * the hand-over RECORD at PS5_WARM_RECORD (12 GiB + 0xF0000): reserved RAM that neither the firmware nor Windows is
//    given. Stage 1 fills it at every run (entry, boot_params, frame buffer); it also carries the arming flag, the loop
//    counter and the last stub step from one loop to the next. A launch from Linux starts it unarmed, loop 0.
//  * the BLOCK: 16 pages of EfiRuntimeServicesData below 4 GiB that this firmware allocates at boot. Windows maps it for
//    our runtime code and never uses it. It holds the stub, its GDT and identity page tables, and copies of the record's
//    values (the record itself is not mapped while Windows runs).
//
// Copyright (c) 2026 ps5win contributors

#ifndef PS5_WARM_H_
#define PS5_WARM_H_

#define PS5_WARM_RECORD        0x3000F0000ULL
#define PS5_WARM_RECORD_MAGIC  0x3143455257355350ULL      // "PS5WREC1"
#define PS5_WARM_BLOCK_MAGIC   0x314D5257U                // "WRM1"
#define PS5_WARM_BLOCK_PAGES   16
#define PS5_WARM_CAP_DEFAULT   20                         // loops before Restart falls back to the ICC restart
#define PS5_WARM_TEST_3A       0x1                        // record flag: firmware-only loop test (launch option ps5warm=3a)
#define PS5_WARM_DIAG_SIZE     0x110                      // = WPD_SIZE (warm_pci.h; checked in Ps5Warm.c)

// Where stage 1, its boot_params and the bundle live (kexec's segments; reserved, never given to UEFI or Windows).
#define PS5_WARM_STAGE_LO      0x7A000000ULL
#define PS5_WARM_STAGE_HI      0x7F000000ULL
// The VRAM carve-out (system RAM the GPU scans out from).
#define PS5_WARM_CARVE_TOP     0x470000000ULL

// Record (physical, written by stage 1; field offsets also used by build_stage.sh's stub.c patch and Ps5WarmAsm.S).
#define REC_MAGIC     0x00     // UINT64
#define REC_ENTRY     0x08     // UINT64 stage 1 entry (startup_64)
#define REC_BP        0x10     // UINT64 boot_params
#define REC_FB        0x18     // UINT64 frame buffer the display scans out (in the carve-out)
#define REC_WIDTH     0x20     // UINT32
#define REC_HEIGHT    0x24     // UINT32
#define REC_PITCH     0x28     // UINT32 bytes per line
#define REC_FLAGS     0x2C     // UINT32 PS5_WARM_TEST_*
#define REC_GOPFB     0x30     // UINT64 address the GOP was given (BAR0 alias or the carve-out address)
#define REC_CARVE     0x38     // UINT64 carve-out base
#define REC_ARMED     0x40     // UINT32 0 = off; 1 = Restart loops, set by Windows in this start; n >= 2 = Restart loops,
                               //   inherited from the start before: n - 1 starts in a row that Windows did not confirm
// A start counts as confirmed when Windows sets the Ps5WarmRestart variable to 1 (ps5guard does at every start). A
// Windows that dies before that (0x7B with automatic restart after a stop error) leaves the inherited value, which
// grows by one per start. Above PS5_WARM_UNCONFIRMED_MAX the hook declines and the console restarts normally, so a
// Windows that cannot start ends there after a few tries instead of looping up to the cap (2026-11-24).
#define PS5_WARM_UNCONFIRMED_MAX  4   // armed values 1..5 loop; 6 = the fifth failed start in a row: normal restart
#define PS5_WARM_ARMED_HANDED     0x100   // added to the armed value when the restart was handed to the boot CPU from
                                          // another CPU (warm_hand.h); taken off again by the next run, logged by BDS
#define PS5_WARM_HAND_TICKS       1597000000ULL   // one second of the console's TSC: how long the boot CPU gets
#define REC_LOOPS     0x44     // UINT32 warm loops since the launch from Linux
#define REC_CAP       0x48     // UINT32
#define REC_RUNS      0x4C     // UINT32 stage 1 runs since the launch
#define REC_BLOCK     0x50     // UINT64 this run's block (written by the firmware)
#define REC_STEP      0x58     // UINT32 last stub step reached (screen mark number)
#define REC_SIZE      0x60

// Block header (block-relative).
#define BLK_MAGIC     0x000    // UINT32
#define BLK_MODE      0x048    // UINT32 1 = jump to stage 1
#define BLK_ENTRY     0x050    // UINT64
#define BLK_RSI       0x058    // UINT64 boot_params
#define BLK_FB        0x060    // UINT64
#define BLK_PITCH     0x068    // UINT32
#define BLK_WIDTH     0x06C    // UINT32
#define BLK_HEIGHT    0x070    // UINT32
#define BLK_RECORD    0x078    // UINT64
#define BLK_ARMED     0x080    // UINT32
#define BLK_LOOPS     0x084    // UINT32
#define BLK_CAP       0x088    // UINT32
#define BLK_HAND      0x08C    // UINT32 hand-over word (warm_hand.h: 1 offered, 2 taken, 3 withdrawn)
#define BLK_BSP       0x090    // UINT32 APIC id of the boot CPU (set at boot)
#define BLK_GDT       0x100
#define BLK_GDTR      0x180
#define BLK_IDTR      0x190    // diagnostics: exceptions 0-31 -> a red square, the vector in binary, then a halt loop
#define BLK_IDT       0x200    // 32 interrupt gates
#define BLK_DIAG      0xC00    // copy of the record's PCI restore report (warm_pci.h WPD_*, 0x110 bytes) for Windows
#define BLK_BLOB      0x1000
#define BLK_PML4      0x2000
#define BLK_PDPT      0x3000   // 0-512 GiB: 0-4 GiB through four page directories, the rest in 1 GiB pages
#define BLK_PD        0x4000   // four page directories, 2 MiB pages
#define BLK_SW_PDPT   0x8000   // tables for the one page of the switch code at the OS's virtual address
#define BLK_SW_PD     0x9000
#define BLK_SW_PT     0xA000
#define BLK_PDPT2     0xB000   // 512 GiB - 1 TiB, 1 GiB pages
#define BLK_STACK_TOP 0x10000

#ifndef __ASSEMBLER__
VOID     Ps5WarmInit (VOID);                     // constructor, boot time
VOID     Ps5WarmConvert (VOID);                  // SetVirtualAddressMap
VOID     Ps5WarmTry (IN BOOLEAN AfterExitBootServices);   // ResetWarm/ResetCold: returns only if declined
EFI_STATUS Ps5WarmSetArmed (IN UINT8 Armed);     // runtime, from the Ps5WarmRestart variable
BOOLEAN  Ps5WarmStatus (OUT UINT32 *Status4);    // {valid, armed, loops, cap}
UINTN    Ps5WarmDiag (OUT UINT8 *Buffer);        // stage 1's PCI restore report of this run; returns its size (PS5_WARM_DIAG_SIZE)
VOID     Ps5WarmMarkDeclined (VOID);             // Ps5Power reached: red square 15 (only while identity-mapped)
#endif

#endif
