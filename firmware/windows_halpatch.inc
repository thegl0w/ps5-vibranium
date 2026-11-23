// SPDX-License-Identifier: MIT
// HAL I/O APIC probe patch (ntoskrnl 10.0.19041 LTSC 2021 build in boot.wim, HalpApicInitializeIoUnit).
// The PS5 has no I/O APIC; Windows' HAL refuses a MADT without one (bugcheck 0x5C/0x200), and a RAM page fails
// its probe because the probe writes 0 to the read-only version register and reads it back (0x5C/0x203).
// At ExitBootServices the kernel image is in memory: find the probe and turn its two "IOWIN = 0" writes into
// 7-byte NOPs, so a RAM page holding 0x00170011 at +0x10 reads as version 0x11 with 24 lines.
// Only loader-owned memory is scanned. The block occurs in the mapped kernel and in a raw copy of the file
// (2 hits in the VM); every copy is patched when there are 1 to 4 hits, none otherwise.
// 2026-11-22: the 44-byte block is recognised by its FNV-1a fingerprint (no bytes of the kernel stored here). It is,
// twice, "IOREGSEL = 1; IOWIN = 0; IOREGSEL = 1; read IOWIN": each IOREGSEL write is mov dword [rbx], imm32 (C7 03 ..), the
// two writes at +6 and +28 are mov dword [rbx+0x10], 0 (7 bytes each).
#define HAL_PROBE_LEN  44U
#define HAL_PROBE_FNV  0xb52d15455196d1b1ULL
STATIC UINT64 HalFnv64 (CONST UINT8 *P, UINTN N)
{
  UINT64 H = 0xcbf29ce484222325ULL;
  for (UINTN I = 0; I < N; ++I) {
    H ^= P[I];
    H *= 0x100000001b3ULL;
  }
  return H;
}
STATIC CONST UINT8 mNop7[7] = { 0x0F,0x1F,0x80,0x00,0x00,0x00,0x00 };
STATIC UINT8  mHalMapBuf[64 * 1024];
STATIC UINT64 mHalPatchAt;
STATIC UINT32 mHalPatchHits;
STATIC UINT8 *mHalHits[4];
STATIC EFI_GET_MEMORY_MAP mHalGetMap;   // the original GetMemoryMap when it is hooked

// Called with boot services still up (inside the ExitBootServices hook, before the real call). No allocation.
STATIC VOID HalPatchIoApicProbe (VOID)
{
  UINTN Size = sizeof (mHalMapBuf), Key, Ds;
  UINT32 Ver;
  mHalPatchHits = 0;
  EFI_GET_MEMORY_MAP Get = mHalGetMap != NULL ? mHalGetMap : gBS->GetMemoryMap;
  if (EFI_ERROR (Get (&Size, (EFI_MEMORY_DESCRIPTOR *)mHalMapBuf, &Key, &Ds, &Ver))) return;
  for (UINTN O = 0; O + Ds <= Size; O += Ds) {
    EFI_MEMORY_DESCRIPTOR *D = (VOID *)(mHalMapBuf + O);
    if (D->Type != EfiLoaderCode && D->Type != EfiLoaderData) continue;
    UINT8 *P = (UINT8 *)(UINTN)D->PhysicalStart;
    UINTN Len = EFI_PAGES_TO_SIZE (D->NumberOfPages);
    for (UINTN I = 0; I + HAL_PROBE_LEN <= Len; ++I) {
      if (P[I] == 0xC7 && P[I + 1] == 0x03 && HalFnv64 (P + I, HAL_PROBE_LEN) == HAL_PROBE_FNV) {
        if (mHalPatchHits < 4) mHalHits[mHalPatchHits] = P + I;
        ++mHalPatchHits;
      }
    }
  }
  if (mHalPatchHits == 0 || mHalPatchHits > 4) return;
  for (UINT32 H = 0; H < mHalPatchHits; ++H) {
    CopyMem (mHalHits[H] + 6, mNop7, 7);
    CopyMem (mHalHits[H] + 28, mNop7, 7);
  }
  mHalPatchAt = (UINT64)(UINTN)mHalHits[0];
}
