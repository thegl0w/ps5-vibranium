// SPDX-License-Identifier: MIT
//
// Warm restart (see Ps5Warm.h): boot-time set-up of the block and the runtime decision in ResetWarm/ResetCold.
// Adapted from warmrestart/exp2/warmrt.c (proven under OVMF and under Windows in KVM, 2026-11-23).
//
// Restart re-enters stage 1 only when every check passes: a valid record from stage 1, 1 GiB pages available, the
// block allocated, armed (variable Ps5WarmRestart = 1, off after every launch from Linux; inherited over a warm run for
// at most PS5_WARM_UNCONFIRMED_MAX starts that Windows did not confirm), loop counter below the cap,
// Windows' address of the switch code mappable, and running on the boot CPU; on another CPU the restart is handed
// over to the boot CPU (warm_hand.h), and only if that fails the ICC restart follows. Otherwise ResetWarm/ResetCold go on to
// the ICC restart exactly as before.
//
// Copyright (c) 2026 ps5win contributors

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>

#include "Ps5Warm.h"
#include "Ps5WarmConfig.h"          // PS5_WARM_ENABLED, written by firmware/prepare.py (PS5WARM=1)
#define WP_NO_CODE                  // layout of stage 1's PCI restore report only
#include "warm_pci.h"

STATIC_ASSERT (WPD_SIZE == PS5_WARM_DIAG_SIZE, "report size");
STATIC_ASSERT (BLK_DIAG + WPD_SIZE <= BLK_BLOB, "report fits in front of the blob");

extern VOID EFIAPI Ps5WarmSwitch (UINT64 Cr3, UINT64 Blob);
extern UINT32 EFIAPI Ps5WarmKick (UINT64 Cr3, UINT64 HandPhys, UINT32 BootCpu, UINT64 Ticks);
extern VOID EFIAPI Ps5WarmNmiEntry (VOID);
extern volatile UINT64 gPs5WarmHand[3];

// Hand-over to the boot CPU when the reset arrives on another one (warm_hand.h, shared with the emulator test).
typedef UINT8 u8; typedef UINT32 u32; typedef UINT64 u64;
#define WH_SIDT(p)   AsmReadIdtr ((IA32_DESCRIPTOR *)(VOID *)(p))
#define WH_RDCR0()   ((u64)AsmReadCr0 ())
#define WH_WRCR0(v)  AsmWriteCr0 ((UINTN)(v))
#define WH_KICK(pml4, hand, bsp, ticks)  Ps5WarmKick ((pml4), (hand), (bsp), (ticks))
#include "warm_hand.h"
extern UINT8  Ps5WarmBlob[], Ps5WarmBlobEnd[], Ps5WarmVecs[];

STATIC UINT8            *mPs5WarmBlk   = NULL;   // as this code reaches it (converted at SetVirtualAddressMap)
STATIC volatile UINT64  mPs5WarmBlkPhys = 0;     // plain numbers, never converted; volatile so the compiler cannot
STATIC volatile UINT64  mPs5WarmSwPhys  = 0;     // replace the switch's physical address by its current address
STATIC BOOLEAN          mPs5WarmReady   = FALSE;

#define R32(p, o)  (*(volatile UINT32 *)((UINT8 *)(p) + (o)))
#define R64(p, o)  (*(volatile UINT64 *)((UINT8 *)(p) + (o)))

//
// Diagnostic squares from C (same row as the stub's: 40x40 at y = 40, x = 40 + 56 * slot), drawn only while this code
// runs identity-mapped (a firmware-only test such as 3a; under Windows the record and the frame buffer are not mapped).
//
STATIC
VOID
Ps5WarmBox (
  IN UINT32  Slot,
  IN UINT32  Color
  )
{
  volatile UINT8  *Rec = (volatile UINT8 *)(UINTN)PS5_WARM_RECORD;
  volatile UINT8  *Fb;
  UINT32          P, X, Y;

  if (!mPs5WarmReady || ((UINT64)(UINTN)&Ps5WarmSwitch != mPs5WarmSwPhys) || (R64 (Rec, REC_MAGIC) != PS5_WARM_RECORD_MAGIC)) {
    return;
  }

  R32 (Rec, REC_STEP) = 0x200 + Slot;
  Fb = (volatile UINT8 *)(UINTN)R64 (Rec, REC_GOPFB);
  P  = R32 (Rec, REC_PITCH);
  if (Fb == NULL) {
    return;
  }

  for (Y = 40; Y < 80; Y++) {
    for (X = 40 + 56 * Slot; X < 80 + 56 * Slot; X++) {
      *(volatile UINT32 *)(Fb + (UINTN)Y * P + X * 4) = Color;
    }
  }
}

VOID
Ps5WarmMarkDeclined (
  VOID
  )
{
  Ps5WarmBox (15, 0xFFFF0000);
}

STATIC
BOOLEAN
Ps5WarmRecordSane (
  IN volatile UINT8  *Rec
  )
{
  UINT64  Entry = R64 (Rec, REC_ENTRY), Bp = R64 (Rec, REC_BP), Fb = R64 (Rec, REC_FB), Carve = R64 (Rec, REC_CARVE);
  UINT32  W = R32 (Rec, REC_WIDTH), H = R32 (Rec, REC_HEIGHT), P = R32 (Rec, REC_PITCH);
  UINT32  Cap = R32 (Rec, REC_CAP), Loops = R32 (Rec, REC_LOOPS);

  return (BOOLEAN)(
    (R64 (Rec, REC_MAGIC) == PS5_WARM_RECORD_MAGIC) &&
    (Entry >= PS5_WARM_STAGE_LO) && (Entry < PS5_WARM_STAGE_HI) &&
    (Bp >= PS5_WARM_STAGE_LO) && (Bp < PS5_WARM_STAGE_HI) &&
    (Carve >= 0x300000000ULL) && (Carve < PS5_WARM_CARVE_TOP) && ((Carve & 0xFFFFF) == 0) &&
    (W >= 640) && (W <= 3840) && (H >= 480) && (H <= 2160) && (P >= W * 4) && (P <= 16384) && ((P & 3) == 0) &&
    (Fb >= Carve) && ((Fb & 0xFFF) == 0) && (Fb + (UINT64)P * H <= PS5_WARM_CARVE_TOP) &&
    (Cap >= 1) && (Cap <= 1000) && (Loops <= Cap));
}

/**
  Constructor (boot time, identity-mapped): if stage 1 left a valid record, build this run's block.
**/
VOID
Ps5WarmInit (
  VOID
  )
{
  volatile UINT8        *Rec = (volatile UINT8 *)(UINTN)PS5_WARM_RECORD;
  EFI_PHYSICAL_ADDRESS  B    = 0xFFFFFFFFULL;
  UINT8                 *Blk;
  UINT64                *Gdt, *Pml4, *Pdpt, *Pdpt2, *Pd;
  UINT32                Eax, Ebx, Ecx, Edx;
  UINT64                CarveLo, G, I, Flags;
  UINTN                 BlobSize = (UINTN)(Ps5WarmBlobEnd - Ps5WarmBlob);

  if (!PS5_WARM_ENABLED || !Ps5WarmRecordSane (Rec) || (BlobSize > 0x1000)) {
    return;
  }

  AsmCpuid (0x80000001, &Eax, &Ebx, &Ecx, &Edx);
  if ((Edx & BIT26) == 0) {
    return;                                         // no 1 GiB pages: the record and the carve-out could not be mapped
  }

  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiRuntimeServicesData, PS5_WARM_BLOCK_PAGES, &B))) {
    return;
  }

  Blk = (UINT8 *)(UINTN)B;
  ZeroMem (Blk, PS5_WARM_BLOCK_PAGES * EFI_PAGE_SIZE);

  Gdt    = (UINT64 *)(Blk + BLK_GDT);
  Gdt[1] = 0x00CF9A000000FFFFULL;                   // 0x08 code, 32-bit
  Gdt[2] = 0x00CF92000000FFFFULL;                   // 0x10 data
  Gdt[3] = 0x00AF9A000000FFFFULL;                   // 0x18 code, 64-bit
  *(UINT16 *)(Blk + BLK_GDTR)     = 4 * 8 - 1;
  *(UINT64 *)(Blk + BLK_GDTR + 2) = B + BLK_GDT;

  //
  // Diagnostic exception table: vectors 0-31 -> the stub's 8-byte entry points (Ps5WarmVecs, inside the copied blob).
  //
  for (I = 0; I < 32; I++) {
    UINT64  H  = B + BLK_BLOB + (UINT64)(Ps5WarmVecs - Ps5WarmBlob) + I * 8;
    UINT64  *E = (UINT64 *)(Blk + BLK_IDT + I * 16);
    E[0] = (H & 0xFFFF) | (0x18ULL << 16) | (0x8EULL << 40) | (((H >> 16) & 0xFFFF) << 48);
    E[1] = H >> 32;
  }

  *(UINT16 *)(Blk + BLK_IDTR)     = 32 * 16 - 1;
  *(UINT64 *)(Blk + BLK_IDTR + 2) = B + BLK_IDT;

  //
  // Identity map of the first 1 TiB. 0-2 GiB write-back (RAM); 2-4 GiB uncached (device registers, local APIC);
  // 4 GiB up to the carve-out write-back (RAM, the record); the carve-out uncached (the squares must reach the
  // screen); above it uncached (no RAM there).
  //
  CarveLo = R64 (Rec, REC_CARVE) >> 30;
  Pml4    = (UINT64 *)(Blk + BLK_PML4);
  Pdpt    = (UINT64 *)(Blk + BLK_PDPT);
  Pdpt2   = (UINT64 *)(Blk + BLK_PDPT2);
  Pml4[0] = (B + BLK_PDPT) | 3;
  Pml4[1] = (B + BLK_PDPT2) | 3;
  for (G = 0; G < 4; G++) {
    Pd      = (UINT64 *)(Blk + BLK_PD + G * EFI_PAGE_SIZE);
    Pdpt[G] = (B + BLK_PD + G * EFI_PAGE_SIZE) | 3;
    for (I = 0; I < 512; I++) {
      Pd[I] = ((G << 30) | (I << 21)) | 0x83 | ((G >= 2) ? 0x18 : 0);
    }
  }

  for (G = 4; G < 1024; G++) {
    Flags = 0x83 | ((G >= CarveLo) ? 0x18 : 0);
    if (G < 512) {
      Pdpt[G] = (G << 30) | Flags;
    } else {
      Pdpt2[G - 512] = (G << 30) | Flags;
    }
  }

  CopyMem (Blk + BLK_BLOB, Ps5WarmBlob, BlobSize);
  //
  // What stage 1 found and did to the PCI state on a warm run (zero after a launch from Linux): kept in the block,
  // because the record is not mapped while Windows runs. Read through the Ps5WarmRestart variable.
  //
  CopyMem (Blk + BLK_DIAG, (VOID *)(UINTN)(PS5_WARM_RECORD + WPD_OFF), WPD_SIZE);

  *(UINT32 *)(Blk + BLK_MODE)   = 1;
  *(UINT64 *)(Blk + BLK_ENTRY)  = R64 (Rec, REC_ENTRY);
  *(UINT64 *)(Blk + BLK_RSI)    = R64 (Rec, REC_BP);
  *(UINT64 *)(Blk + BLK_FB)     = R64 (Rec, REC_FB);
  *(UINT32 *)(Blk + BLK_PITCH)  = R32 (Rec, REC_PITCH);
  *(UINT32 *)(Blk + BLK_WIDTH)  = R32 (Rec, REC_WIDTH);
  *(UINT32 *)(Blk + BLK_HEIGHT) = R32 (Rec, REC_HEIGHT);
  *(UINT64 *)(Blk + BLK_RECORD) = PS5_WARM_RECORD;
  //
  // Armed carries over a warm run (a launch from Linux wrote 0), one higher: this start is not confirmed until
  // Windows sets the variable to 1 (Ps5WarmSetArmed). See REC_ARMED in Ps5Warm.h.
  //
  {
    UINT32  A = R32 (Rec, REC_ARMED) & ~(UINT32)PS5_WARM_ARMED_HANDED;   // the mark of a hand-over is for the log only
    *(UINT32 *)(Blk + BLK_ARMED) = ((A >= 1) && (A <= PS5_WARM_UNCONFIRMED_MAX + 1)) ? A + 1 : 0;
  }
  AsmCpuid (1, &Eax, &Ebx, &Ecx, &Edx);
  *(UINT32 *)(Blk + BLK_BSP) = Ebx >> 24;           // this is the boot CPU: the only one the stub may run on
  *(UINT32 *)(Blk + BLK_LOOPS)  = R32 (Rec, REC_LOOPS);
  *(UINT32 *)(Blk + BLK_CAP)    = R32 (Rec, REC_CAP);
  *(UINT32 *)(Blk + BLK_MAGIC)  = PS5_WARM_BLOCK_MAGIC;

  R64 (Rec, REC_BLOCK) = B;                         // for the boot-time test (BDS) and for a look after a hang
  mPs5WarmBlk     = Blk;
  mPs5WarmBlkPhys = B;
  mPs5WarmSwPhys  = (UINT64)(UINTN)&Ps5WarmSwitch;
  mPs5WarmReady   = TRUE;
}

VOID
Ps5WarmConvert (
  VOID
  )
{
  if (mPs5WarmReady && EFI_ERROR (EfiConvertPointer (0, (VOID **)&mPs5WarmBlk))) {
    mPs5WarmReady = FALSE;
  }
}

EFI_STATUS
Ps5WarmSetArmed (
  IN UINT8  Armed
  )
{
  if (!mPs5WarmReady || (R32 (mPs5WarmBlk, BLK_MAGIC) != PS5_WARM_BLOCK_MAGIC)) {
    return EFI_UNSUPPORTED;
  }

  if (Armed > 1) {
    return EFI_INVALID_PARAMETER;
  }

  R32 (mPs5WarmBlk, BLK_ARMED) = Armed;
  return EFI_SUCCESS;
}

BOOLEAN
Ps5WarmStatus (
  OUT UINT32  *Status4
  )
{
  BOOLEAN  Ok = (BOOLEAN)(mPs5WarmReady && (R32 (mPs5WarmBlk, BLK_MAGIC) == PS5_WARM_BLOCK_MAGIC));

  Status4[0] = Ok ? 1 : 0;
  Status4[1] = Ok ? R32 (mPs5WarmBlk, BLK_ARMED) : 0;
  Status4[2] = Ok ? R32 (mPs5WarmBlk, BLK_LOOPS) : 0;
  Status4[3] = Ok ? R32 (mPs5WarmBlk, BLK_CAP) : 0;
  return Ok;
}

UINTN
Ps5WarmDiag (
  OUT UINT8  *Buffer
  )
{
  if (mPs5WarmReady && (R32 (mPs5WarmBlk, BLK_MAGIC) == PS5_WARM_BLOCK_MAGIC)) {
    CopyMem (Buffer, mPs5WarmBlk + BLK_DIAG, PS5_WARM_DIAG_SIZE);
  } else {
    ZeroMem (Buffer, PS5_WARM_DIAG_SIZE);
  }

  return PS5_WARM_DIAG_SIZE;
}

/**
  ResetWarm / ResetCold. Returns only when the warm path is not taken (the caller then sends the ICC restart).
**/
VOID
Ps5WarmTry (
  IN BOOLEAN  AfterExitBootServices
  )
{
  UINT8             *Blk = mPs5WarmBlk;
  UINT64            Va, Pa;
  volatile UINT64   *Pml4, *SwPdpt, *SwPd, *SwPt;

  if (!AfterExitBootServices || !mPs5WarmReady || (Blk == NULL)) {
    return;
  }

  Ps5WarmBox (11, 0xFFFF8000);                      // orange: hook entered (diagnostic, identity-mapped runs only)

  if ((R32 (Blk, BLK_MAGIC) != PS5_WARM_BLOCK_MAGIC) || (R32 (Blk, BLK_ARMED) < 1) ||
      (R32 (Blk, BLK_ARMED) > PS5_WARM_UNCONFIRMED_MAX + 1) ||    // too many starts in a row that Windows never confirmed
      (R32 (Blk, BLK_LOOPS) >= R32 (Blk, BLK_CAP)))
  {
    return;
  }

  Va = (UINT64)(UINTN)&Ps5WarmSwitch;
  Pa = mPs5WarmSwPhys;
  if (Va != Pa) {
    //
    // The OS runs this code at a virtual address. Map that one page in our tables too, so the instruction after
    // "mov cr3" is still there; everything after it runs at physical addresses. PML4 slots 0 and 1 are our identity map.
    //
    if (((Va >> 39) & 511) <= 1) {
      return;
    }

    Pml4   = (volatile UINT64 *)(Blk + BLK_PML4);
    SwPdpt = (volatile UINT64 *)(Blk + BLK_SW_PDPT);
    SwPd   = (volatile UINT64 *)(Blk + BLK_SW_PD);
    SwPt   = (volatile UINT64 *)(Blk + BLK_SW_PT);
    ZeroMem ((VOID *)(Blk + BLK_SW_PDPT), 3 * EFI_PAGE_SIZE);
    Pml4[(Va >> 39) & 511]  = (mPs5WarmBlkPhys + BLK_SW_PDPT) | 3;
    SwPdpt[(Va >> 30) & 511] = (mPs5WarmBlkPhys + BLK_SW_PD) | 3;
    SwPd[(Va >> 21) & 511]   = (mPs5WarmBlkPhys + BLK_SW_PT) | 3;
    SwPt[(Va >> 12) & 511]   = (Pa & ~0xFFFULL) | 1;
  }

  if ((AsmReadMsr64 (0x1B) & BIT8) == 0) {
    //
    // Not the boot CPU (Windows restarts after a stop error on the CPU that hit it). The stub parks every other CPU
    // with INIT, which would send the boot CPU to the reset vector, so it must run ON the boot CPU: offer it the
    // restart through an NMI. Returns here only if the boot CPU did not take over; then the ICC restart follows.
    //
    if ((((UINT64)(UINTN)&Ps5WarmKick ^ Va) >> 12) != 0 || (((UINT64)(UINTN)&Ps5WarmNmiEntry ^ Va) >> 12) != 0) {
      return;                                       // must share the one page mapped above
    }

    R32 (Blk, BLK_ARMED) |= PS5_WARM_ARMED_HANDED;
    wh_handover (mPs5WarmBlkPhys + BLK_PML4, mPs5WarmBlkPhys + BLK_BLOB, mPs5WarmBlkPhys + BLK_HAND,
      (volatile u32 *)(Blk + BLK_HAND), gPs5WarmHand, (UINT64)(UINTN)&Ps5WarmNmiEntry, R32 (Blk, BLK_BSP),
      PS5_WARM_HAND_TICKS);
    return;
  }

  Ps5WarmBox (12, 0xFFFF80C0);                      // pink: every check passed, switching now
  Ps5WarmSwitch (mPs5WarmBlkPhys + BLK_PML4, mPs5WarmBlkPhys + BLK_BLOB);
}
