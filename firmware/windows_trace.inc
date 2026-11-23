// SPDX-License-Identifier: MIT
#include <Pi/PiDxeCis.h>
#include <Guid/DxeServices.h>
#include <Protocol/Cpu.h>
// ---- Boot 9 trace: what Windows Boot Manager and winload do before ExitBootServices.
// Text-mode boot apps print through ConOut; we copy that text, plus USB reads, GOP mode
// changes, key waits and memory-map calls, into a preallocated file on WININSTALL.
// If Windows waits for a key (an error screen) or resets, the framebuffer is saved too.
// Writes happen only in the caller's context at TPL_APPLICATION, in place inside the
// preallocated file (no FAT allocation), and never after ExitBootServices.
#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include "WindowsHalPatch.h"

#define TRACE_BYTES  SIZE_1MB
#define TSC_PER_MS   1596300ULL
STATIC EFI_FILE_PROTOCOL *mTraceFile;
STATIC CHAR8   mTraceBuf[32768];
STATIC UINTN   mTraceLen, mTraceOff;
STATIC UINT32  mTraceLost, mMapCalls, mKeyCalls, mWaitCalls, mReads;
STATIC BOOLEAN mTraceBusy, mExited;
STATIC UINTN   mInRead;
#define SHOT_SLOT    (8 * SIZE_1MB)
#define SHOT_SLOTS   3
STATIC EFI_FILE_PROTOCOL *mShotFile;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *mShotGop;
STATIC UINT32  mShots;
STATIC UINT64  mFirstWait;
STATIC UINT64  mT0, mLastFlush, mLastKey, mLastWait, mUsbBytes, mUsbNext;

STATIC UINT32 TraceMs (VOID) { return (UINT32)DivU64x64Remainder (AsmReadTsc () - mT0, TSC_PER_MS, NULL); }

STATIC VOID TraceRaw (CONST CHAR8 *S, UINTN N)
{
  if (mTraceBusy || mExited) return;
  if (N > sizeof (mTraceBuf) - mTraceLen) { mTraceLost += (UINT32)N; return; }
  CopyMem (mTraceBuf + mTraceLen, S, N);
  mTraceLen += N;
}

STATIC VOID EFIAPI TraceEvent (CONST CHAR8 *Fmt, ...)
{
  CHAR8 Line[200];
  VA_LIST Va;
  if (mTraceBusy || mExited) return;
  UINTN N = AsciiSPrint (Line, sizeof (Line), "\r\n{%u ms} ", TraceMs ());
  VA_START (Va, Fmt);
  N += AsciiVSPrint (Line + N, sizeof (Line) - N, Fmt, Va);
  VA_END (Va);
  TraceRaw (Line, N);
}

STATIC VOID TraceFlush (BOOLEAN Force)
{
  if (mTraceFile == NULL || mExited || mTraceBusy || mInRead != 0 || mTraceLen == 0) return;
  if (!Force && AsmReadTsc () - mLastFlush < 250 * TSC_PER_MS && mTraceLen < sizeof (mTraceBuf) / 2) return;
  EFI_TPL Old = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  gBS->RestoreTPL (Old);
  if (Old > TPL_APPLICATION) return;
  mTraceBusy = TRUE;
  if (mTraceLost != 0 && mTraceLen + 32 < sizeof (mTraceBuf)) {
    mTraceLen += AsciiSPrint (mTraceBuf + mTraceLen, 32, "\r\n{lost %u bytes}", mTraceLost);
    mTraceLost = 0;
  }
  UINTN Size = MIN (mTraceLen, TRACE_BYTES - mTraceOff);
  if (Size != 0 && !EFI_ERROR (mTraceFile->SetPosition (mTraceFile, mTraceOff)) &&
      !EFI_ERROR (mTraceFile->Write (mTraceFile, &Size, mTraceBuf))) {
    mTraceFile->Flush (mTraceFile);
    mTraceOff += Size;
  }
  mTraceLen = 0;
  mLastFlush = AsmReadTsc ();
  mTraceBusy = FALSE;
}

STATIC VOID TraceCrc (EFI_TABLE_HEADER *Hdr)
{
  UINT32 Crc = 0;
  Hdr->CRC32 = 0;
  gBS->CalculateCrc32 (Hdr, Hdr->HeaderSize, &Crc);
  Hdr->CRC32 = Crc;
}

// Save the visible framebuffer (subsampled to fit a slot) as: "PS5SHOT1", UINT32 width,
// height, step, then 32 bytes of reason, then width*height BGRX pixels.
STATIC VOID TraceShot (CONST CHAR8 *Why)
{
  if (mShotFile == NULL || mShotGop == NULL || mShots >= SHOT_SLOTS || mExited || mTraceBusy || mInRead != 0) return;
  EFI_TPL Old = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  gBS->RestoreTPL (Old);
  if (Old > TPL_APPLICATION) return;
  UINT32 W = mShotGop->Mode->Info->HorizontalResolution, H = mShotGop->Mode->Info->VerticalResolution, Step = 1;
  if (W == 0 || H == 0) return;
  while ((UINT64)(W / Step) * (H / Step) * 4 + 64 > SHOT_SLOT) ++Step;
  UINT32 OW = W / Step, OH = H / Step;
  UINTN OutSize = 64 + (UINTN)OW * OH * 4;
  UINT8 *Out = AllocateZeroPool (OutSize);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Row = AllocatePool ((UINTN)W * sizeof (*Row));
  EFI_STATUS St = (Out == NULL || Row == NULL) ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
  for (UINT32 Y = 0; !EFI_ERROR (St) && Y < OH; ++Y) {
    St = mShotGop->Blt (mShotGop, Row, EfiBltVideoToBltBuffer, 0, Y * Step, 0, 0, W, 1, 0);
    for (UINT32 X = 0; !EFI_ERROR (St) && X < OW; ++X) {
      CopyMem (Out + 64 + ((UINTN)Y * OW + X) * 4, &Row[X * Step], 4);
    }
  }
  if (!EFI_ERROR (St)) {
    mTraceBusy = TRUE;
    CopyMem (Out, "PS5SHOT1", 8);
    CopyMem (Out + 8, &OW, 4);
    CopyMem (Out + 12, &OH, 4);
    CopyMem (Out + 16, &Step, 4);
    AsciiStrnCpyS ((CHAR8 *)Out + 20, 32, Why, 31);
    UINTN Size = OutSize;
    St = mShotFile->SetPosition (mShotFile, (UINT64)mShots * SHOT_SLOT);
    if (!EFI_ERROR (St)) St = mShotFile->Write (mShotFile, &Size, Out);
    if (!EFI_ERROR (St)) St = mShotFile->Flush (mShotFile);
    mTraceBusy = FALSE;
  }
  if (Out != NULL) FreePool (Out);
  if (Row != NULL) FreePool (Row);
  TraceEvent ("Screenshot %u (%a): %ux%u step %u, %r ", mShots, Why, OW, OH, Step, St);
  ++mShots;
  TraceFlush (TRUE);
}

// Windows waiting on input usually means an error or recovery screen is up.
STATIC VOID TraceWaiting (VOID)
{
  UINT64 Now = AsmReadTsc ();
  if (mFirstWait == 0) mFirstWait = Now;
  if (mShots == 0 && Now - mFirstWait > 3000 * TSC_PER_MS) TraceShot ("input wait +3 s");
  if (mShots == 1 && Now - mFirstWait > 30000 * TSC_PER_MS) TraceShot ("input wait +30 s");
}

#include "WindowsKstubInstall.h"

// ConOut
STATIC EFI_TEXT_STRING       mOrigOut;
STATIC EFI_TEXT_CLEAR_SCREEN mOrigCls;
STATIC EFI_TEXT_SET_MODE     mOrigTextMode;
STATIC EFI_STATUS EFIAPI HookOut (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *S)
{
  EFI_STATUS St = mOrigOut (This, S);
  if (S != NULL && !mTraceBusy && !mExited) {
    CHAR8 A[256];
    UINTN N = 0;
    BOOLEAN Nl = FALSE;
    for (; *S != 0 && N < sizeof (A); ++S) {
      CHAR16 C = *S;
      if (C == L'\n') Nl = TRUE;
      A[N++] = (C == L'\r' || C == L'\n' || (C >= 0x20 && C < 0x7f)) ? (CHAR8)C : '?';
    }
    TraceRaw (A, N);
    if (Nl) TraceFlush (FALSE);
  }
  return St;
}
STATIC EFI_STATUS EFIAPI HookCls (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This)
{
  TraceEvent ("ConOut ClearScreen ");
  return mOrigCls (This);
}
STATIC EFI_STATUS EFIAPI HookTextMode (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Mode)
{
  EFI_STATUS St = mOrigTextMode (This, Mode);
  TraceEvent ("ConOut SetMode %u: %r ", (UINT32)Mode, St);
  return St;
}

// ConIn: a boot-manager error screen waits for a key.
STATIC EFI_INPUT_READ_KEY mOrigKey;
STATIC EFI_STATUS EFIAPI HookKey (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key)
{
  ++mKeyCalls;
  TraceWaiting ();
  if (!mExited && AsmReadTsc () - mLastKey > 1000 * TSC_PER_MS) {
    mLastKey = AsmReadTsc ();
    TraceEvent ("ReadKeyStroke polled, %u calls ", mKeyCalls);
    TraceFlush (TRUE);
  }
  return mOrigKey (This, Key);
}

// Boot services
STATIC EFI_WAIT_FOR_EVENT     mOrigWait;
STATIC EFI_STALL              mOrigStall;
STATIC EFI_GET_MEMORY_MAP     mOrigMap;
STATIC EFI_EXIT_BOOT_SERVICES mOrigEbs;
STATIC EFI_RESET_SYSTEM       mOrigReset;
STATIC EFI_STATUS EFIAPI HookWait (UINTN Count, EFI_EVENT *Events, UINTN *Index)
{
  ++mWaitCalls;
  TraceWaiting ();
  if (!mExited && AsmReadTsc () - mLastWait > 1000 * TSC_PER_MS) {
    mLastWait = AsmReadTsc ();
    TraceEvent ("WaitForEvent(%u events, ConIn=%d), %u calls ", (UINT32)Count,
      (Count > 0 && Events != NULL && gST->ConIn != NULL && Events[0] == gST->ConIn->WaitForKey), mWaitCalls);
    TraceFlush (TRUE);
  }
  return mOrigWait (Count, Events, Index);
}
STATIC EFI_STATUS EFIAPI HookStall (UINTN Us)
{
  // Inside USB transfers this is skipped by the mInRead and TPL guards in TraceFlush.
  if (Us >= 100000) TraceEvent ("Stall %u us ", (UINT32)Us);
  if (Us >= 1000) TraceFlush (FALSE);
  return mOrigStall (Us);
}
// Boot 19: restart and power-off. The reset library in ResetSystemRuntimeDxe (firmware/ps5reset) sends Linux's ICC power message
// when Windows calls the UEFI ResetSystem service. It needs the two mailbox pages of the Salina glue function mapped at runtime:
// mark them EFI_MEMORY_RUNTIME so they appear in the memory map as runtime MMIO, and log what the mailbox looks like now (reads only).
STATIC VOID Ps5ResetPrepare (VOID)
{
  STATIC CONST EFI_PHYSICAL_ADDRESS Pages[2] = { 0x85308000ULL, 0x85400000ULL };
  EFI_DXE_SERVICES *Ds = NULL;
  if (mKstubForceDebugcon || mKstubForceGpuTest) return;                    // VM builds have no such device
  if (EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiDxeServicesTableGuid, (VOID **)&Ds)) || Ds == NULL) {
    TraceEvent ("reset: no DXE services table ");
    return;
  }
  for (UINTN I = 0; I < 2; ++I) {
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
    EFI_STATUS S0 = Ds->GetMemorySpaceDescriptor (Pages[I], &D), S1 = EFI_NOT_STARTED, S2 = EFI_NOT_STARTED;
    if (!EFI_ERROR (S0)) {
      S1 = Ds->SetMemorySpaceCapabilities (Pages[I], SIZE_4KB, D.Capabilities | EFI_MEMORY_RUNTIME);
      S2 = Ds->SetMemorySpaceAttributes (Pages[I], SIZE_4KB, D.Attributes | EFI_MEMORY_RUNTIME);
    }
    TraceEvent ("reset: page 0x%Lx GCD %r type %u caps 0x%Lx attrs 0x%Lx, runtime caps %r attrs %r ", Pages[I], S0,
      EFI_ERROR (S0) ? 0 : (UINT32)D.GcdMemoryType, EFI_ERROR (S0) ? 0 : D.Capabilities, EFI_ERROR (S0) ? 0 : D.Attributes, S1, S2);
  }
  UINT32 Saved = IoRead32 (0xCF8), Cfg[4];
  STATIC CONST UINT8 Regs[4] = { 0x00, 0x04, 0x18, 0x20 };
  for (UINTN I = 0; I < 4; ++I) {
    IoWrite32 (0xCF8, 0x80000000U | (1 << 11) | (1 << 8) | Regs[I]);
    Cfg[I] = IoRead32 (0xCFC);
  }
  IoWrite32 (0xCF8, Saved);
  volatile UINT8 *Bell = (volatile UINT8 *)(UINTN)Pages[0], *Icc = (volatile UINT8 *)(UINTN)Pages[1];
  TraceEvent ("reset: bridge 00:01.1 id 0x%x cmd/status 0x%x buses 0x%x mem window 0x%x ", Cfg[0], Cfg[1], Cfg[2], Cfg[3]);
  TraceEvent ("reset: ICC as found: intr status 0x%x mask 0x%x, SOW 0x%x SOR 0x%x EMW 0x%x EMR 0x%x ",
    *(volatile UINT32 *)(Bell + 0x14), *(volatile UINT32 *)(Bell + 0x24), *(volatile UINT16 *)(Icc + 0x7F0),
    *(volatile UINT16 *)(Icc + 0x7F4), *(volatile UINT16 *)(Icc + 0xFF0), *(volatile UINT16 *)(Icc + 0xFF4));
}
// Boot 16: clear EFER bit 16 (see windows_kstub_install.inc). The write is guarded: a #GP on the WRMSR is skipped and reported.
STATIC volatile BOOLEAN mEferGp;
STATIC VOID EFIAPI EferGpHandler (EFI_EXCEPTION_TYPE Type, EFI_SYSTEM_CONTEXT Ctx)
{
  UINT8 *Ip = (UINT8 *)(UINTN)Ctx.SystemContextX64->Rip;
  if (Ip[0] != 0x0F || Ip[1] != 0x30) CpuDeadLoop ();      // not our WRMSR
  Ctx.SystemContextX64->Rip += 2;
  mEferGp = TRUE;
}
STATIC BOOLEAN EferWriteGuarded (UINT64 Value)
{
  EFI_CPU_ARCH_PROTOCOL *Cpu;
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu)) ||
      EFI_ERROR (Cpu->RegisterInterruptHandler (Cpu, 13, EferGpHandler))) {
    mEferState = 4;
    return FALSE;
  }
  mEferGp = FALSE;
  AsmWriteMsr64 (0xC0000080, Value);
  Cpu->RegisterInterruptHandler (Cpu, 13, NULL);
  return !mEferGp;
}
STATIC VOID EferClearXotext (VOID)
{
  mEferBefore = mEferAfter = AsmReadMsr64 (0xC0000080);
  if ((mEferBefore & BIT16) == 0) {
    mEferState = 1;
  } else if (EferWriteGuarded (mEferBefore & ~(UINT64)BIT16)) {
    mEferAfter = AsmReadMsr64 (0xC0000080);
    mEferState = (mEferAfter & BIT16) == 0 ? 2 : 3;
  } else if (mEferState != 4) {
    mEferState = 3;
  }
  TraceEvent ("EFER 0x%Lx -> 0x%Lx, bit 16 (xotext enable) state %u (1 already clear, 2 cleared, 3 refused, 4 no handler) ",
    mEferBefore, mEferAfter, mEferState);
}
// Boot 15: what Windows asks the firmware for. Windows' ramdisk (0x101BF000) and kernel image (0x2800000) sat inside the
// range this firmware reports as reserved, so log its large or failed AllocatePages calls and the loader-owned ranges.
STATIC EFI_ALLOCATE_PAGES mOrigAlloc;
STATIC UINT32 mAllocCalls, mAllocFails, mAllocLogged;
// Boot 15: the DXE core turns "reserved" resource HOBs whose attributes are present|initialized (untested memory) into
// free RAM as soon as one allocation fails (PromoteMemoryResource); loading bootmgfw.efi (ImageBase 0x10000000) did that
// on every boot so far, and Windows then used all reserved ranges as RAM. make_bundle.py now marks them present-only.
// Second line of defence: every range that is reserved when the trace starts stays reserved in the map Windows sees,
// and fixed-address allocations inside one are refused.
STATIC struct { UINT64 Lo, Hi; } mProt[32];
STATIC UINT32 mProtCount, mProtConv, mProtUsed, mProtRefused, mProtLogs;
STATIC BOOLEAN ProtHit (UINT64 Lo, UINT64 Hi)
{
  for (UINT32 I = 0; I < mProtCount; ++I) {
    if (Lo < mProt[I].Hi && Hi > mProt[I].Lo) return TRUE;
  }
  return FALSE;
}
// Boot 21: memory below 1 MiB for Windows. The HAL starts the other processors from a stub it allocates below 1 MiB
// (HalpSetupAcpiPhase0 -> HalpAllocPhysicalMemory with a 0x100000 limit); our UEFI map has no RAM there, so only one processor
// could ever start. Linux uses 0x1000-0x6FFFF as ordinary RAM. That range is not given to EDK2 (its MP code would look for a
// wake-up buffer there and might start the other processors itself); instead it is added to the map Windows sees, and the
// few firmware calls Windows could make about it (fixed-address or below-1-MiB loader allocations, frees) are answered here.
#define LOW_PAGES 0x100
STATIC UINT64 mLowLo, mLowHi;                 // page aligned, inside 0x1000-0xFFFFF; mLowHi = 0 means off
STATIC UINT64 mLowWantLo, mLowWantHi;         // set before TraceInstall to choose the range (VM tests)
STATIC UINT8  mLowType[LOW_PAGES];            // EFI memory type per page
STATIC UINT32 mLowAllocs, mLowFrees, mLowRefused, mLowLogs;
STATIC UINT8 mLowReportType = EfiConventionalMemory;
STATIC BOOLEAN mLowHideReal;                  // VM tests: report everything the firmware has below 1 MiB as reserved (the PS5 has nothing there)
STATIC BOOLEAN LowRunFree (UINT64 Start, UINT64 Pages)
{
  if (mLowHi == 0 || Pages == 0 || Start < mLowLo || Start + EFI_PAGES_TO_SIZE (Pages) > mLowHi || (Start & 0xFFF) != 0) return FALSE;
  for (UINT64 P = Start >> 12; P < (Start >> 12) + Pages; ++P) if (mLowType[P] != EfiConventionalMemory) return FALSE;
  return TRUE;
}
STATIC VOID LowMark (UINT64 Start, UINT64 Pages, EFI_MEMORY_TYPE T)
{
  for (UINT64 P = Start >> 12; P < (Start >> 12) + Pages; ++P) mLowType[P] = (UINT8)T;
}
STATIC EFI_FREE_PAGES mOrigFree;
STATIC EFI_STATUS EFIAPI HookFree (EFI_PHYSICAL_ADDRESS Mem, UINTN Pages)
{
  if (mLowHi != 0 && Pages != 0 && Mem >= mLowLo && Mem + EFI_PAGES_TO_SIZE ((UINT64)Pages) <= mLowHi && (Mem & 0xFFF) == 0) {
    LowMark (Mem, Pages, EfiConventionalMemory);
    ++mLowFrees;
    return EFI_SUCCESS;
  }
  return mOrigFree (Mem, Pages);
}
STATIC EFI_STATUS EFIAPI HookAlloc (EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemType, UINTN Pages, EFI_PHYSICAL_ADDRESS *Mem)
{
  EFI_PHYSICAL_ADDRESS In = Mem != NULL ? *Mem : 0;
  if (mLowHi != 0 && Mem != NULL && (MemType == EfiLoaderCode || MemType == EfiLoaderData)) {
    UINT64 Bytes = EFI_PAGES_TO_SIZE ((UINT64)Pages);
    if (Type == AllocateAddress && In < mLowHi && In + Bytes > mLowLo) {
      BOOLEAN Ok = LowRunFree (In, Pages);
      if (Ok) { LowMark (In, Pages, MemType); ++mLowAllocs; } else { ++mLowRefused; }
      if (mLowLogs++ < 24) TraceEvent ("low memory: fixed allocation 0x%Lx pages at 0x%Lx type %u: %a ", (UINT64)Pages, In, (UINT32)MemType, Ok ? "given" : "refused");
      return Ok ? EFI_SUCCESS : EFI_NOT_FOUND;
    }
    if (Type == AllocateMaxAddress && In < SIZE_1MB && Pages != 0 && Bytes <= mLowHi - mLowLo) {
      UINT64 Top = MIN ((In + 1) & ~0xFFFULL, mLowHi);
      for (UINT64 S = Top >= Bytes ? Top - Bytes : 0; S >= mLowLo && S + Bytes <= Top; S -= SIZE_4KB) {
        if (LowRunFree (S, Pages)) {
          LowMark (S, Pages, MemType);
          ++mLowAllocs;
          *Mem = S;
          if (mLowLogs++ < 24) TraceEvent ("low memory: 0x%Lx pages below 0x%Lx type %u: given at 0x%Lx ", (UINT64)Pages, In, (UINT32)MemType, S);
          return EFI_SUCCESS;
        }
        if (S < SIZE_4KB) break;
      }
      ++mLowRefused;
      if (mLowLogs++ < 24) TraceEvent ("low memory: 0x%Lx pages below 0x%Lx type %u: none free ", (UINT64)Pages, In, (UINT32)MemType);
      return EFI_OUT_OF_RESOURCES;
    }
  }
  if (Type == AllocateAddress && Mem != NULL && ProtHit (In, In + EFI_PAGES_TO_SIZE ((UINT64)Pages))) {
    if (mProtRefused++ < 16) TraceEvent ("AllocatePages refused: 0x%Lx pages at 0x%Lx is inside a reserved range ", (UINT64)Pages, In);
    return EFI_NOT_FOUND;
  }
  EFI_STATUS St = mOrigAlloc (Type, MemType, Pages, Mem);
  if (MemType == EfiLoaderCode || MemType == EfiLoaderData) {
    ++mAllocCalls;
    if (EFI_ERROR (St)) ++mAllocFails;
    if ((EFI_ERROR (St) || Pages >= 256) && mAllocLogged < 160) {
      ++mAllocLogged;
      TraceEvent ("AllocatePages type %u mem %u pages 0x%Lx in 0x%Lx out 0x%Lx: %r ", (UINT32)Type, (UINT32)MemType,
        (UINT64)Pages, In, (Mem != NULL && !EFI_ERROR (St)) ? *Mem : 0, St);
    }
  }
  return St;
}
// Optional rewrite of the map Windows sees (VM experiments; a PS5 fix if a type turns out to be respected).
STATIC UINT64 mMapRwLo, mMapRwHi;      // descriptors of type EfiReservedMemoryType inside [Lo, Hi) ...
STATIC UINT32 mMapRwType;              // ... are reported with this type
STATIC UINT64 mMapRwAttr;              // ... and this attribute
STATIC BOOLEAN mMapRwOn;
STATIC VOID TraceLoaderRanges (VOID)
{
  UINTN Size = sizeof (mHalMapBuf), Key, Ds;
  UINT32 Ver, Lines = 0;
  if (EFI_ERROR (mOrigMap (&Size, (EFI_MEMORY_DESCRIPTOR *)mHalMapBuf, &Key, &Ds, &Ver))) return;
  UINT64 Lo = 0, Hi = 0, Total = 0;
  for (UINTN O = 0; O + Ds <= Size + Ds; O += Ds) {
    EFI_MEMORY_DESCRIPTOR *D = (VOID *)(mHalMapBuf + O);
    BOOLEAN Last = O + Ds > Size;
    BOOLEAN Ldr = !Last && (D->Type == EfiLoaderCode || D->Type == EfiLoaderData);
    if (Ldr && Hi == D->PhysicalStart && Hi != 0) { Hi += EFI_PAGES_TO_SIZE (D->NumberOfPages); continue; }
    if (Hi != 0) {
      Total += Hi - Lo;
      if (Lines++ < 80) TraceEvent ("loader range 0x%Lx-0x%Lx ", Lo, Hi - 1);
      Lo = Hi = 0;
    }
    if (Ldr) { Lo = D->PhysicalStart; Hi = Lo + EFI_PAGES_TO_SIZE (D->NumberOfPages); }
  }
  TraceEvent ("loader memory: %u ranges, %Lu KiB; AllocatePages (loader types) %u calls, %u failed ", Lines, Total >> 10, mAllocCalls, mAllocFails);
}
// Flush before the real call so the map key handed back is the current one.
STATIC EFI_STATUS EFIAPI HookMap (UINTN *Size, EFI_MEMORY_DESCRIPTOR *Map, UINTN *Key, UINTN *DSize, UINT32 *Ver)
{
  ++mMapCalls;
  TraceEvent ("GetMemoryMap #%u, USB %Lu KiB read ", mMapCalls, mUsbBytes >> 10);
  // The kernel is loaded by the final burst of GetMemoryMap calls before ExitBootServices (#11-#18 on boots 10/11).
  if (mHalPatchAt == 0 && mMapCalls >= 11) {
    HalPatchIoApicProbe ();
    TraceEvent ("HAL I/O APIC probe patch: %u hits, first at 0x%Lx ", mHalPatchHits, mHalPatchAt);
    if (mHalPatchAt != 0) {
      mKstubStatus = KstubInstallAny ();
      TraceEvent ("Kernel stub: %r (%a), image 0x%Lx, mode %u, fboff 0x%x, pitch %u, sampler %u period 0x%x ", mKstubStatus, mKstubBuild != NULL ? mKstubBuild->Name : mKpatUsed ? "pattern stub" : "no build matched", mKstubBase, mKstubMode, mKstubFbOff, mKstubPitch, mKstubSampler, mKstubPeriod);
      if (mKstubBuild == NULL) TraceEvent ("Pattern stub: %a (check %u), entry 0x%x, cave 0x%x+0x%x, HalpLocalApic 0x%x ", mKpatUsed ? "installed" : "NOT installed", mKpatWhy, mKpatEntryRva, mKpatCaveRva, mKpatCaveLen, mKpatApicRva);
    }
  }
  if (mMapCalls == 11 || mMapCalls == 14) TraceLoaderRanges ();
  TraceFlush (TRUE);
  UINTN Capacity = Size != NULL ? *Size : 0;
  EFI_STATUS St = mOrigMap (Size, Map, Key, DSize, Ver);
  if (mLowHideReal && !EFI_ERROR (St) && Map != NULL) {
    for (UINTN O = 0; O + *DSize <= *Size; O += *DSize) {
      EFI_MEMORY_DESCRIPTOR *D = (VOID *)((UINT8 *)Map + O);
      if (D->PhysicalStart + EFI_PAGES_TO_SIZE (D->NumberOfPages) <= SIZE_1MB) { D->Type = EfiReservedMemoryType; D->Attribute = 0; }
    }
  }
  if (!EFI_ERROR (St) && Map != NULL && mProtCount != 0) {
    UINT32 Conv = 0, Used = 0;
    for (UINTN O = 0; O + *DSize <= *Size; O += *DSize) {
      EFI_MEMORY_DESCRIPTOR *D = (VOID *)((UINT8 *)Map + O);
      if (D->Type == EfiReservedMemoryType || D->Type == EfiMemoryMappedIO ||
          !ProtHit (D->PhysicalStart, D->PhysicalStart + EFI_PAGES_TO_SIZE (D->NumberOfPages))) continue;
      if (D->Type == EfiConventionalMemory) {
        D->Type = EfiReservedMemoryType;
        D->Attribute = 0;
        ++Conv;
      } else {
        ++Used;
      }
    }
    if (Conv != mProtConv || Used != mProtUsed || mProtLogs == 0 || (mProtLogs == 1 && mMapCalls >= 11)) {
      mProtConv = Conv; mProtUsed = Used; mProtLogs = mMapCalls >= 11 ? 2 : 1;
      TraceEvent ("reserved ranges at GetMemoryMap #%u: %u kept; %u free descriptors inside them hidden, %u in use; %u fixed allocations refused ",
        mMapCalls, mProtCount, Conv, Used, mProtRefused);
    }
  }
  if (!EFI_ERROR (St) && mMapRwOn && Map != NULL) {
    for (UINTN O = 0; O + *DSize <= *Size; O += *DSize) {
      EFI_MEMORY_DESCRIPTOR *D = (VOID *)((UINT8 *)Map + O);
      if (D->Type == EfiReservedMemoryType && D->PhysicalStart >= mMapRwLo &&
          D->PhysicalStart + EFI_PAGES_TO_SIZE (D->NumberOfPages) <= mMapRwHi) {
        D->Type = mMapRwType;
        D->Attribute = mMapRwAttr;
      }
    }
  }
  // Low memory last: the reserved-range guard above must not see (and hide) the descriptors added here.
  if (mLowHi != 0 && Size != NULL && DSize != NULL && *DSize != 0) {
    if (St == EFI_BUFFER_TOO_SMALL) {
      *Size += 24 * *DSize;                       // room for the low-memory descriptors added below
    } else if (!EFI_ERROR (St) && Map != NULL) {
      // drop what the firmware itself reports inside the range (VM tests reserve the real memory there), then add ours
      UINTN Out = 0;
      for (UINTN O = 0; O + *DSize <= *Size; O += *DSize) {
        EFI_MEMORY_DESCRIPTOR *D = (VOID *)((UINT8 *)Map + O);
        if (D->PhysicalStart >= mLowLo && D->PhysicalStart + EFI_PAGES_TO_SIZE (D->NumberOfPages) <= mLowHi) continue;
        if (Out != O) CopyMem ((UINT8 *)Map + Out, D, *DSize);
        Out += *DSize;
      }
      UINTN Runs = 0;
      for (UINT64 P = mLowLo >> 12; P < mLowHi >> 12; ++P) if (P == mLowLo >> 12 || mLowType[P] != mLowType[P - 1]) ++Runs;
      if (Out + Runs * *DSize > Capacity) {
        *Size = Out + (Runs + 8) * *DSize;
        return EFI_BUFFER_TOO_SMALL;
      }
      for (UINT64 P = mLowLo >> 12; P < mLowHi >> 12; ) {
        UINT64 E = P + 1;
        while (E < mLowHi >> 12 && mLowType[E] == mLowType[P]) ++E;
        EFI_MEMORY_DESCRIPTOR *D = (VOID *)((UINT8 *)Map + Out);
        ZeroMem (D, *DSize);
        D->Type = mLowType[P];
        D->PhysicalStart = P << 12;
        D->NumberOfPages = E - P;
        D->Attribute = EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB;
        Out += *DSize;
        P = E;
      }
      *Size = Out;
      if (mMapCalls == 2 || mMapCalls == 11 || mMapCalls == 14) {
        TraceEvent ("low memory 0x%Lx-0x%Lx at GetMemoryMap #%u: %u descriptors, %u allocations, %u frees, %u refused ",
          mLowLo, mLowHi - 1, mMapCalls, (UINT32)Runs, mLowAllocs, mLowFrees, mLowRefused);
      }
    }
  }
  return St;
}
STATIC VOID EFIAPI HookReset (EFI_RESET_TYPE Type, EFI_STATUS Status, UINTN Size, VOID *Data)
{
  TraceEvent ("ResetSystem type %u, %r ", (UINT32)Type, Status);
  if (Data != NULL && Size >= 2) {
    CHAR8 A[96];
    UINTN N = 0;
    for (CHAR16 *S = Data; *S != 0 && N < sizeof (A) && (UINTN)((UINT8 *)S - (UINT8 *)Data) + 2 <= Size; ++S) {
      A[N++] = (*S >= 0x20 && *S < 0x7f) ? (CHAR8)*S : '?';
    }
    TraceRaw (A, N);
  }
  TraceFlush (TRUE);
  TraceShot ("ResetSystem");
  mOrigReset (Type, Status, Size, Data);
}
// AMD extended APIC (ported from the macOS fix, ps5-hackintosh/tools/make_ierfix.py): after the crash hand-off,
// IERE (0x410 bit 0) is set and IER0..7 (0x480 + 16 n) mask almost every vector (IER6 = 0x1f), so the timer and IPI
// vectors stay in IRR and are never delivered. Windows, like XNU, does not know these registers. Linux also leaves
// the core performance counters (PerfCtl 0xC0010200 + 2 i) running with an overflow interrupt; zero them.
STATIC volatile UINT32 *ApicMmio (VOID)
{
  UINT64 Base = AsmReadMsr64 (0x1B);
  if ((Base & BIT11) == 0 || (Base & BIT10) != 0) return NULL;   // disabled, or x2APIC mode
  return (volatile UINT32 *)(UINTN)(Base & 0xFFFFFF000ULL);
}
STATIC VOID ApicLog (CONST CHAR8 *When)
{
  volatile UINT32 *L = ApicMmio ();
  if (L == NULL) {
    TraceEvent ("APIC %a: not in xAPIC MMIO mode, base MSR 0x%Lx ", When, AsmReadMsr64 (0x1B));
    return;
  }
  TraceEvent ("APIC %a: id 0x%x ver 0x%x extfeat 0x%x extctl(IERE) 0x%x svr 0x%x lvtpc 0x%x lvtt 0x%x ", When, L[0x20 / 4], L[0x30 / 4],
    L[0x400 / 4], L[0x410 / 4], L[0xF0 / 4], L[0x340 / 4], L[0x320 / 4]);
  TraceEvent ("APIC %a: IER 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x ", When, L[0x480 / 4], L[0x490 / 4], L[0x4A0 / 4],
    L[0x4B0 / 4], L[0x4C0 / 4], L[0x4D0 / 4], L[0x4E0 / 4], L[0x4F0 / 4]);
  TraceEvent ("APIC %a: ISR6 0x%x IRR6 0x%x IRR7 0x%x PerfCtl0-5 0x%Lx 0x%Lx 0x%Lx 0x%Lx 0x%Lx 0x%Lx ", When, L[0x160 / 4], L[0x260 / 4],
    L[0x270 / 4], AsmReadMsr64 (0xC0010200), AsmReadMsr64 (0xC0010202), AsmReadMsr64 (0xC0010204), AsmReadMsr64 (0xC0010206),
    AsmReadMsr64 (0xC0010208), AsmReadMsr64 (0xC001020A));
}
STATIC VOID ApicOpenForWindows (VOID)
{
  volatile UINT32 *L = ApicMmio ();
  for (UINT32 I = 0; I < 6; ++I) AsmWriteMsr64 (0xC0010200 + 2 * I, 0);
  // Only with extended APIC space (version bit 31) and IER capability (ext feature bit 0); IER accepts writes only while IERE is set.
  if (L != NULL && (L[0x30 / 4] & BIT31) != 0 && (L[0x400 / 4] & BIT0) != 0 && (L[0x410 / 4] & 1) != 0) {
    for (UINT32 N = 0; N < 8; ++N) L[(0x480 + 16 * N) / 4] = 0xFFFFFFFF;
  }
  // NMI sampler of the kernel stub: PerfCtl is zero here, so nothing fires until the stub arms PERF_CTR0 from
  // KiInitializeKernel on (kernel IDT loaded). LVTPC = NMI delivery, unmasked; the HAL masks it later and the stub re-opens it.
  if (L != NULL && mKstubStatus == EFI_SUCCESS && mKstubMode != 0 && mKstubSampler != 0) L[0x340 / 4] = 0x400;
}

// Installer proposal (2026-11-21): without the stub, Windows runs its 15 application processors with Linux's PerfCtl
// counters armed and extended-APIC vectors 0xC5-0xDF masked (the 2026-11-20/21 compositor corruption). A release
// build stops instead of booting like that: whole screen red, reason in the trace, wait for the power button.
// run_stage.sh checks the kernel against the same table before the hand-over, so this is the second wall.
// Build with PS5_KSTUB_OPTIONAL to keep the old behaviour (boot on without the stub) for experiments.
STATIC VOID KstubRequire (VOID)
{
#ifndef PS5_KSTUB_OPTIONAL
  if (mKstubStatus == EFI_SUCCESS) return;
  TraceEvent ("STOP: kernel stub %r, %u HAL probe hits: this ntoskrnl has no table stub and the pattern stub failed check %u ", mKstubStatus, mHalPatchHits, mKpatWhy);
  TraceFlush (TRUE);
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)) && Gop != NULL) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL Red = { 0x00, 0x00, 0xC0, 0x00 };
    Gop->Blt (Gop, &Red, EfiBltVideoFill, 0, 0, 0, 0, Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution, 0);
  }
  {
    STATIC CONST CHAR16 *Why[] = { L"-", L"kernel image not found (exactly one HAL probe hit inside a PE image needed)",
                                   L"entry point bytes differ", L"no unique, unused POOLCODE cave",
                                   L"local-APIC accessors not found exactly once each" };
    Print (L"\r\n\r\n  STOPPED: this Windows kernel has no kernel stub in this firmware, and the pattern stub refused it.\r\n"
           L"  Reason: %s (check %u). Nothing was patched. Hold the power button to turn the console off.\r\n",
           mKpatWhy < 5 ? Why[mKpatWhy] : L"?", mKpatWhy);
  }
  CpuDeadLoop ();
#endif
}

STATIC EFI_STATUS EFIAPI HookEbs (EFI_HANDLE Image, UINTN Key)
{
  // Windows owns the runtime table after this; put the real ResetSystem back first.
  gRT->ResetSystem = mOrigReset;
  TraceCrc (&gRT->Hdr);
  if (mHalPatchAt == 0) HalPatchIoApicProbe ();
  if (mHalPatchAt != 0 && mKstubStatus == EFI_NOT_STARTED) mKstubStatus = KstubInstallAny ();
  KstubRequire ();
  TraceEvent ("ExitBootServices (not persisted) ");
  EFI_STATUS St = mOrigEbs (Image, Key);
  if (!EFI_ERROR (St)) {
    mExited = TRUE;
    ApicOpenForWindows ();      // firmware timer is stopped now; Windows owns the APIC from here
    if (mEferState == 2 && (AsmReadMsr64 (0xC0000080) & BIT16) != 0) {      // something set it again: the write is known to work
      AsmWriteMsr64 (0xC0000080, AsmReadMsr64 (0xC0000080) & ~(UINT64)BIT16);
    }
    KstubPortRect (40, 1290, 56, 36, 0x00FF00FF);       // magenta: port drawing still works after ExitBootServices
    // Boot 16, next to it: EFER bit 16 at hand-off. Cyan = was set, now clear; white = was already clear; red = still set.
    KstubPortRect (112, 1290, 56, 36, (AsmReadMsr64 (0xC0000080) & BIT16) != 0 ? 0x00FF0000 : mEferState == 2 ? 0x0000FFFF : 0x00FFFFFF);
  }
  return St;
}

// GOP
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *mGop[4];
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE mGopSet[4];
STATIC UINTN mGops;
STATIC EFI_STATUS EFIAPI HookGopSet (EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 Mode)
{
  for (UINTN I = 0; I < mGops; ++I) {
    if (mGop[I] == This) {
      EFI_STATUS St = mGopSet[I] (This, Mode);
      TraceEvent ("GOP%u SetMode %u: %r, now %ux%u ", (UINT32)I, Mode, St,
        This->Mode->Info->HorizontalResolution, This->Mode->Info->VerticalResolution);
      TraceFlush (FALSE);
      return St;
    }
  }
  return EFI_UNSUPPORTED;
}

// USB block reads
STATIC EFI_BLOCK_IO_PROTOCOL *mBio[8];
STATIC EFI_BLOCK_READ mBioRead[8];
STATIC UINTN mBios;
// Boot 10: every outermost USB read goes through this firmware buffer (below 4 GiB),
// so the controller never DMAs into memory chosen by the Windows boot manager.
#define BOUNCE_BYTES (4 * SIZE_1MB)
STATIC UINT8 *mBounce;
STATIC EFI_STATUS EFIAPI HookRead (EFI_BLOCK_IO_PROTOCOL *This, UINT32 Media, EFI_LBA Lba, UINTN Size, VOID *Buf)
{
  for (UINTN I = 0; I < mBios; ++I) {
    if (mBio[I] == This) {
      // Early reads: persist the request first, so a read that never returns is still on disk.
      if (!mTraceBusy && !mExited && mReads < 300) {
        TraceEvent ("read start dev%u lba 0x%Lx size 0x%x buf %p ", (UINT32)I, Lba, (UINT32)Size, Buf);
        TraceFlush (TRUE);
      }
      EFI_STATUS St = EFI_SUCCESS;
      UINT32 Bs = This->Media->BlockSize;
      if (mBounce != NULL && mInRead == 0 && Bs != 0 && Size % Bs == 0 && !mExited) {
        ++mInRead;
        for (UINTN Done = 0; Done < Size && !EFI_ERROR (St); ) {
          UINTN Chunk = MIN (Size - Done, BOUNCE_BYTES - BOUNCE_BYTES % Bs);
          St = mBioRead[I] (This, Media, Lba + Done / Bs, Chunk, mBounce);
          if (!EFI_ERROR (St)) CopyMem ((UINT8 *)Buf + Done, mBounce, Chunk);
          Done += Chunk;
        }
        --mInRead;
      } else {
        ++mInRead;
        St = mBioRead[I] (This, Media, Lba, Size, Buf);
        --mInRead;
      }
      if (!mTraceBusy && !mExited) {
        mUsbBytes += Size;
        ++mReads;
        if (EFI_ERROR (St)) {
          TraceEvent ("READ ERROR dev%u lba 0x%Lx size 0x%x: %r ", (UINT32)I, Lba, (UINT32)Size, St);
          TraceFlush (TRUE);
        } else if (mReads <= 300) {
          TraceEvent ("read done dev%u: %r ", (UINT32)I, St);
          TraceFlush (FALSE);
        } else if (mUsbBytes >= mUsbNext) {
          mUsbNext = mUsbBytes + 16 * SIZE_1MB;
          TraceEvent ("USB %Lu MiB read in %u calls, last dev%u lba 0x%Lx ", mUsbBytes >> 20, mReads, (UINT32)I, Lba);
          TraceFlush (FALSE);
        }
      }
      return St;
    }
  }
  return EFI_DEVICE_ERROR;
}

// Create a fresh, zero-filled file (never overwrite an earlier run) and keep it open.
STATIC EFI_STATUS TraceCreate (EFI_FILE_PROTOCOL *Root, CONST CHAR16 *Prefix, UINTN Bytes, EFI_FILE_PROTOCOL **Out)
{
  EFI_FILE_PROTOCOL *File = NULL;
  CHAR16 Name[48];
  EFI_STATUS Status = EFI_VOLUME_FULL;
  for (UINTN Run = 1; Run <= 9; ++Run) {
    UnicodeSPrint (Name, sizeof (Name), L"\\%s%u.%s", Prefix, (UINT32)Run, Bytes == TRACE_BYTES ? L"txt" : L"bin");
    Status = Root->Open (Root, &File, Name, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR (Status)) {
      File->Close (File);
      File = NULL;
      Status = EFI_VOLUME_FULL;
      continue;
    }
    Status = Root->Open (Root, &File, Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    break;
  }
  if (File == NULL && Status == EFI_VOLUME_FULL) {
    // 2026-11-22: all 9 slots exist: reuse the last one (Name still holds it) rather than run without a trace.
    Status = Root->Open (Root, &File, Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR (Status)) File = NULL;
  }
  if (File == NULL) return EFI_ERROR (Status) ? Status : EFI_VOLUME_FULL;
  VOID *Zero = AllocateZeroPool (SIZE_1MB);
  Status = Zero == NULL ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
  for (UINTN Off = 0; !EFI_ERROR (Status) && Off < Bytes; Off += SIZE_1MB) {
    UINTN Size = SIZE_1MB;
    Status = File->Write (File, &Size, Zero);
  }
  if (Zero != NULL) FreePool (Zero);
  if (!EFI_ERROR (Status)) Status = File->Flush (File);
  if (EFI_ERROR (Status)) {
    File->Close (File);
    return Status;
  }
  *Out = File;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS TraceOpen (EFI_HANDLE Target)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root;
  EFI_STATUS Status = gBS->HandleProtocol (Target, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) return Status;
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) return Status;
  Status = TraceCreate (Root, L"ps5-boot22-trace", TRACE_BYTES, &mTraceFile);
  if (!EFI_ERROR (Status)) {
    // Screenshots are optional; the trace runs without them.
    if (EFI_ERROR (TraceCreate (Root, L"ps5-boot22-screen", SHOT_SLOT * SHOT_SLOTS, &mShotFile))) mShotFile = NULL;
  }
  Root->Close (Root);
  return Status;
}

STATIC VOID TraceInstall (VOID)
{
  EFI_HANDLE *Handles;
  UINTN Count;
  mT0 = AsmReadTsc ();
  mUsbNext = 16 * SIZE_1MB;
  TraceEvent ("Boot 22 trace: boot 21 + temperatures and fan mode for Windows through two UEFI variable names (runtime, ICC). Screenshot file %a. ", mShotFile != NULL ? "ready" : "missing");
  EferClearXotext ();
  Ps5ResetPrepare ();
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &Count, &Handles))) {
    for (UINTN I = 0; I < Count && mGops < ARRAY_SIZE (mGop); ++I) {
      EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
      if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop))) continue;
      TraceEvent ("GOP%u conout=%d mode %u of %u, fb 0x%Lx size 0x%Lx ", (UINT32)mGops, Handles[I] == gST->ConsoleOutHandle,
        Gop->Mode->Mode, Gop->Mode->MaxMode, Gop->Mode->FrameBufferBase, (UINT64)Gop->Mode->FrameBufferSize);
      for (UINT32 M = 0; M < Gop->Mode->MaxMode && M < 16; ++M) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
        UINTN ISize;
        if (!EFI_ERROR (Gop->QueryMode (Gop, M, &ISize, &Info))) {
          TraceEvent ("  mode %u: %ux%u fmt %u stride %u ", M, Info->HorizontalResolution, Info->VerticalResolution,
            (UINT32)Info->PixelFormat, Info->PixelsPerScanLine);
          FreePool (Info);
        }
      }
      if (mShotGop == NULL || Handles[I] == gST->ConsoleOutHandle) mShotGop = Gop;
      mGop[mGops] = Gop;
      mGopSet[mGops] = Gop->SetMode;
      Gop->SetMode = HookGopSet;
      ++mGops;
    }
    FreePool (Handles);
  }
  if (gST->ConOut != NULL) {
    TraceEvent ("ConOut text mode %d of %d ", gST->ConOut->Mode->Mode, gST->ConOut->Mode->MaxMode);
    mOrigOut = gST->ConOut->OutputString;      gST->ConOut->OutputString = HookOut;
    mOrigCls = gST->ConOut->ClearScreen;       gST->ConOut->ClearScreen = HookCls;
    mOrigTextMode = gST->ConOut->SetMode;      gST->ConOut->SetMode = HookTextMode;
  }
  if (gST->ConIn != NULL) {
    mOrigKey = gST->ConIn->ReadKeyStroke;      gST->ConIn->ReadKeyStroke = HookKey;
  }
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles))) {
    for (UINTN I = 0; I < Count && mBios < ARRAY_SIZE (mBio); ++I) {
      BOOLEAN Usb = FALSE;
      EFI_DEVICE_PATH_PROTOCOL *Dp = DevicePathFromHandle (Handles[I]);
      for (EFI_DEVICE_PATH_PROTOCOL *N = Dp; N != NULL && !IsDevicePathEnd (N); N = NextDevicePathNode (N)) {
        if (DevicePathType (N) == MESSAGING_DEVICE_PATH && DevicePathSubType (N) == MSG_USB_DP) Usb = TRUE;
      }
      EFI_BLOCK_IO_PROTOCOL *Bio;
      if (!Usb || EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiBlockIoProtocolGuid, (VOID **)&Bio))) continue;
      CHAR16 *Text = ConvertDevicePathToText (Dp, TRUE, TRUE);
      TraceEvent ("dev%u: %s, %Lu blocks of %u ", (UINT32)mBios, Text != NULL ? Text : L"?",
        Bio->Media->LastBlock + 1, Bio->Media->BlockSize);
      if (Text != NULL) FreePool (Text);
      mBio[mBios] = Bio;
      mBioRead[mBios] = Bio->ReadBlocks;
      Bio->ReadBlocks = HookRead;
      ++mBios;
    }
    FreePool (Handles);
  }
  {
    EFI_PHYSICAL_ADDRESS Bounce = 0xFFFFFFFF;
    if (!EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, EFI_SIZE_TO_PAGES (BOUNCE_BYTES), &Bounce))) {
      mBounce = (UINT8 *)(UINTN)Bounce;
    }
    TraceEvent ("USB read bounce buffer at 0x%Lx ", (UINT64)(UINTN)mBounce);
  }
  {
    // The memory map Windows starts from, for locating its buffers.
    UINTN Size = 0, Key, Ds = 0;
    UINT32 Ver;
    EFI_MEMORY_DESCRIPTOR *Map = NULL;
    if (gBS->GetMemoryMap (&Size, NULL, &Key, &Ds, &Ver) == EFI_BUFFER_TOO_SMALL && Ds != 0) {
      Size += 16 * Ds;
      Map = AllocatePool (Size);
    }
    if (Map != NULL && !EFI_ERROR (gBS->GetMemoryMap (&Size, Map, &Key, &Ds, &Ver))) {
      for (UINTN O = 0; O < Size; O += Ds) {
        EFI_MEMORY_DESCRIPTOR *D = (VOID *)((UINT8 *)Map + O);
        TraceEvent ("map type %u 0x%Lx-0x%Lx attr 0x%Lx ", D->Type, D->PhysicalStart,
          D->PhysicalStart + EFI_PAGES_TO_SIZE (D->NumberOfPages) - 1, D->Attribute);
        if (D->Type == EfiReservedMemoryType && mProtCount < ARRAY_SIZE (mProt)) {
          mProt[mProtCount].Lo = D->PhysicalStart;
          mProt[mProtCount].Hi = D->PhysicalStart + EFI_PAGES_TO_SIZE (D->NumberOfPages);
          ++mProtCount;
        }
        if (mTraceLen > sizeof (mTraceBuf) / 2) TraceFlush (TRUE);
      }
    }
    if (Map != NULL) FreePool (Map);
  }
  mOrigWait = gBS->WaitForEvent;       gBS->WaitForEvent = HookWait;
  mOrigStall = gBS->Stall;             gBS->Stall = HookStall;
  mOrigMap = gBS->GetMemoryMap;        gBS->GetMemoryMap = HookMap;
  mOrigAlloc = gBS->AllocatePages;     gBS->AllocatePages = HookAlloc;
  mOrigFree = gBS->FreePages;          gBS->FreePages = HookFree;
  // Low memory for Windows: on the PS5 0x1000-0x6FFFF (Linux "System RAM"); VM tests choose their own range.
  if (mLowWantHi == 0 && !mKstubForceDebugcon && !mKstubForceGpuTest) { mLowWantLo = 0x1000; mLowWantHi = 0x70000; }
  if (mLowWantHi != 0 && mLowWantLo >= SIZE_4KB && mLowWantHi <= SIZE_1MB && mLowWantLo < mLowWantHi) {
    mLowLo = mLowWantLo; mLowHi = mLowWantHi;
    // KVM, 16 processors, everything real below 1 MiB hidden: with no usable memory there Windows bugchecks 0x7E (access
    // violation in KeStartAllProcessors); with this range reported as EfiConventionalMemory or as EfiBootServicesData all 16
    // start. Conventional is what a PC firmware reports; Windows then takes one page at 0x1000 through AllocatePages (HookAlloc).
    SetMem (mLowType, sizeof (mLowType), mLowReportType);
    TraceEvent ("low memory: 0x%Lx-0x%Lx offered to Windows as type %u (not known to the firmware's allocator) ", mLowLo, mLowHi - 1, (UINT32)mLowReportType);
  }
  mHalGetMap = mOrigMap;
  mOrigEbs = gBS->ExitBootServices;    gBS->ExitBootServices = HookEbs;
  TraceCrc (&gBS->Hdr);
  mOrigReset = gRT->ResetSystem;       gRT->ResetSystem = HookReset;
  TraceCrc (&gRT->Hdr);
  ApicLog ("at launch");
  KstubPickMode (mShotGop);
  TraceEvent ("Hooks installed. Starting bootmgfw.efi. ");
  TraceFlush (TRUE);
}

