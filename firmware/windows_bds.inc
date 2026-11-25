// SPDX-License-Identifier: MIT
#include "Ps5WarmConfig.h"     // PS5_WARM_ENABLED, PS5_DEV_DISKS, PS5_CARVE_BASE (written by firmware/prepare.py)
#ifndef PS5_CARVE_BASE
#define PS5_CARVE_BASE 0x370000000ULL   // 4 GiB carve-out
#endif
#include <Guid/Acpi.h>
#include <IndustryStandard/Acpi.h>
#include <Protocol/UsbIo.h>
#include <Protocol/PartitionInfo.h>

// Inspect the map Windows will inherit. Gaps are valid for reserved/MMIO.
// Every mapped page in a protected range must have a non-allocatable type.
STATIC BOOLEAN WindowsMapSafe (VOID)
{
  UINTN Size = 0, Key, Stride = 0;
  UINT32 Version;
  if (gBS->GetMemoryMap (&Size, NULL, &Key, &Stride, &Version) != EFI_BUFFER_TOO_SMALL || Stride == 0) return FALSE;
  Size += 8 * Stride;
  EFI_MEMORY_DESCRIPTOR *Map = AllocatePool (Size);
  if (Map == NULL) return FALSE;
  if (EFI_ERROR (gBS->GetMemoryMap (&Size, Map, &Key, &Stride, &Version))) {
    FreePool (Map);
    return FALSE;
  }
  UINTN Count = Size / Stride;
  BOOLEAN Ok = TRUE;
  for (UINTN I = 0; I < Count; ++I) {
    EFI_MEMORY_DESCRIPTOR *D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + I * Stride);
    UINT64 Lo = D->PhysicalStart, Hi = Lo + EFI_PAGES_TO_SIZE (D->NumberOfPages);
    if ((Lo < 0x6f600000 && Hi > 0x6f000000) || (Lo < 0x70600000 && Hi > 0x70000000) ||
        (Lo < 0x470000000ULL && Hi > PS5_CARVE_BASE)) {
      if (D->Type != EfiReservedMemoryType && D->Type != EfiMemoryMappedIO) Ok = FALSE;
    }
  }
  FreePool (Map);
  return Ok;
}

STATIC BOOLEAN WindowsAcpiSafe (VOID)
{
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *Rsdp = NULL;
  EFI_STATUS Status = EfiGetSystemConfigurationTable (&gEfiAcpi20TableGuid, (VOID **)&Rsdp);
  if (EFI_ERROR (Status) || Rsdp == NULL || Rsdp->XsdtAddress == 0) return FALSE;
  EFI_ACPI_DESCRIPTION_HEADER *Xsdt = (VOID *)(UINTN)Rsdp->XsdtAddress;
  if (Xsdt->Length < sizeof (*Xsdt) || Xsdt->Length > 4096) return FALSE;
  UINTN Matches = 0;
  for (UINTN Off = sizeof (*Xsdt); Off + 8 <= Xsdt->Length; Off += 8) {
    EFI_ACPI_DESCRIPTION_HEADER *Table = (VOID *)(UINTN)ReadUnaligned64 ((UINT64 *)((UINT8 *)Xsdt + Off));
    if (Table->Signature == SIGNATURE_32 ('M','C','F','G')) {
      if (Table->Length != 60 || ((UINT8 *)Table)[54] != 0 || ((UINT8 *)Table)[55] != 0x20) return FALSE;
      Matches++;
    }
  }
  return Matches == 1;
}

STATIC EFI_STATUS BootMark (EFI_HANDLE Target, CHAR8 *Message)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root, *File;
  EFI_STATUS Status = gBS->HandleProtocol (Target, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) return Status;
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) return Status;
  Status = Root->Open (Root, &File, L"\\ps5-boot22.txt", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (!EFI_ERROR (Status)) {
    UINTN Length = AsciiStrLen (Message);
    UINTN Size = Length;
    Status = File->SetPosition (File, MAX_UINT64);
    if (!EFI_ERROR (Status)) Status = File->Write (File, &Size, Message);
    if (!EFI_ERROR (Status) && Size != Length) Status = EFI_DEVICE_ERROR;
    if (!EFI_ERROR (Status)) Status = File->Flush (File);
    File->Close (File);
  }
  Root->Close (Root);
  return Status;
}

#include "WindowsMediaCrc.h"

STATIC EFI_STATUS VerifyFile (
  EFI_HANDLE Target, EFI_FILE_PROTOCOL *Root, CHAR16 *Name, UINTN Length,
  CONST UINT32 *Crc, VOID *Buffer
  )
{
  EFI_FILE_PROTOCOL *File;
  CHAR8 Message[160];
  UINTN Offset = 0;
  Print (L"Verifying %s (%u MiB)\r\n", Name, (UINT32)(Length / SIZE_1MB));
  EFI_STATUS Status = Root->Open (Root, &File, Name, EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    while (Offset < Length) {
      UINTN Want = MIN (Length - Offset, SIZE_1MB);
      UINTN Got = Want;
      UINT32 Sum;
      Status = File->Read (File, &Got, Buffer);
      if (EFI_ERROR (Status)) break;
      if (Got != Want) {
        Status = EFI_BAD_BUFFER_SIZE;
        break;
      }
      Status = gBS->CalculateCrc32 (Buffer, Got, &Sum);
      if (EFI_ERROR (Status)) break;
      if (Sum != Crc[Offset / SIZE_1MB]) {
        Status = EFI_CRC_ERROR;
        break;
      }
      Offset += Got;
      if (Offset % (64 * SIZE_1MB) == 0) {
        AsciiSPrint (Message, sizeof (Message), "Boot 22 %s: %u MiB verified.\r\n", Name, (UINT32)(Offset / SIZE_1MB));
        Print (L"  %u MiB verified\r\n", (UINT32)(Offset / SIZE_1MB));
        Status = BootMark (Target, Message);
        if (EFI_ERROR (Status)) break;
      }
    }
    if (!EFI_ERROR (Status)) {
      UINTN Extra = 1;
      Status = File->Read (File, &Extra, Buffer);
      if (!EFI_ERROR (Status) && Extra != 0) Status = EFI_BAD_BUFFER_SIZE;
    }
    File->Close (File);
  }
  AsciiSPrint (Message, sizeof (Message), "Boot 22 %s: %u bytes verified, %r.\r\n", Name, (UINT32)Offset, Status);
  EFI_STATUS LogStatus = BootMark (Target, Message);
  return EFI_ERROR (Status) ? Status : LogStatus;
}

STATIC EFI_STATUS VerifyMedia (EFI_HANDLE Target)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root;
  EFI_STATUS Status = gBS->HandleProtocol (Target, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) return Status;
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) return Status;
  VOID *Buffer = AllocatePool (SIZE_1MB);
  if (Buffer == NULL) {
    Root->Close (Root);
    return EFI_OUT_OF_RESOURCES;
  }
  // Boot 22: Windows Boot Manager rewrites the BCD hive once it gets far enough (Boot 16 set \\Description\\KeyName and new
  // sequence numbers), so a BCD mismatch is logged by VerifyFile but no longer stops the boot.
  VerifyFile (Target, Root, L"\\EFI\\Microsoft\\Boot\\BCD", BCD_SIZE, BcdCrc, Buffer);
  Status = VerifyFile (Target, Root, L"\\boot\\boot.sdi", SDI_SIZE, SdiCrc, Buffer);
  if (!EFI_ERROR (Status)) Status = VerifyFile (Target, Root, L"\\sources\\boot.wim", WIM_SIZE, WimCrc, Buffer);
  FreePool (Buffer);
  Root->Close (Root);
  return Status;
}

#include "WindowsTrace.h"

// Boot 22: TRUE when the file can be opened read-only on that volume.
STATIC BOOLEAN WindowsFileExists (EFI_HANDLE Volume, CHAR16 *Name)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root, *File;
  if (Volume == NULL || EFI_ERROR (gBS->HandleProtocol (Volume, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) ||
      EFI_ERROR (Fs->OpenVolume (Fs, &Root))) return FALSE;
  EFI_STATUS Status = Root->Open (Root, &File, Name, EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) File->Close (File);
  Root->Close (Root);
  return !EFI_ERROR (Status);
}

// TRUE when the handle's device path goes through a USB port.
STATIC BOOLEAN WindowsOnUsb (EFI_HANDLE Handle)
{
  for (EFI_DEVICE_PATH_PROTOCOL *N = DevicePathFromHandle (Handle);
       N != NULL && !IsDevicePathEnd (N); N = NextDevicePathNode (N)) {
    if (DevicePathType (N) == MESSAGING_DEVICE_PATH && DevicePathSubType (N) == MSG_USB_DP) return TRUE;
  }
  return FALSE;
}

STATIC UINTN WindowsCountHandles (EFI_GUID *Protocol, BOOLEAN UsbOnly)
{
  EFI_HANDLE *Handles;
  UINTN Count, Found = 0;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, Protocol, NULL, &Count, &Handles))) return 0;
  for (UINTN I = 0; I < Count; ++I) {
    if (!UsbOnly || WindowsOnUsb (Handles[I])) Found++;
  }
  FreePool (Handles);
  return Found;
}

// One pass over the file systems that exist right now: the three approved USB GPT partitions, if they are there.
STATIC VOID WindowsFindVolumes (EFI_HANDLE *Target, EFI_HANDLE *EfiPart, EFI_HANDLE *SsdPart)
{
  EFI_HANDLE *Handles;
  UINTN Count;
  // Boot 26: Windows' boot files on the Sabrent SSD (300 MB FAT32 partition PS5BOOT, made 2026-11-20). When it is there
  // with a boot manager, it is used for everything (marks, trace, boot) and the Toshiba is not needed.
#if PS5_DEV_DISKS
  // Dev builds only (firmware/prepare.py PS5DEV=1): the development disks' partition GUIDs (SsdBoot, Installer, EfiSys),
  // kept in firmware/dev_disks.inc, which is not published. Release builds find only disks made by the PS5 Windows
  // installer (GPT name PS5BOOT + \ps5win.id), never a fixed disk identity.
#include "WindowsDevDisks.h"
#endif
  // Installer proposal (2026-11-21): a disk made by the PS5 Windows installer has its own partition GUIDs. Its boot
  // partition is found by GPT name "PS5BOOT" plus the installer's record \ps5win.id on it. The fixed GUID above keeps
  // today's Sabrent working. Two boot partitions at once (two PS5 Windows disks attached) stop the boot.
  UINTN Boots = 0;
  *Target = *EfiPart = *SsdPart = NULL;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles))) return;
  for (UINTN I = 0; I < Count; ++I) {
    BOOLEAN Usb = FALSE, ByGuid = FALSE;
    for (EFI_DEVICE_PATH_PROTOCOL *N = DevicePathFromHandle (Handles[I]);
         N != NULL && !IsDevicePathEnd (N); N = NextDevicePathNode (N)) {
      if (DevicePathType (N) == MESSAGING_DEVICE_PATH && DevicePathSubType (N) == MSG_USB_DP) {
        Usb = TRUE;
      }
#if PS5_DEV_DISKS
      if (Usb && DevicePathType (N) == MEDIA_DEVICE_PATH && DevicePathSubType (N) == MEDIA_HARDDRIVE_DP) {
        HARDDRIVE_DEVICE_PATH *Hd = (HARDDRIVE_DEVICE_PATH *)N;
        if (Hd->SignatureType == SIGNATURE_TYPE_GUID && Hd->PartitionNumber == 2 &&
            CompareMem (Hd->Signature, &Installer, sizeof (Installer)) == 0) {
          if (*Target != NULL) {
            Print (L"Duplicate Windows partition. Stopped.\r\n");
            CpuDeadLoop ();
          }
          *Target = Handles[I];
        }
        if (Hd->SignatureType == SIGNATURE_TYPE_GUID && Hd->PartitionNumber == 1 &&
            CompareMem (Hd->Signature, &EfiSys, sizeof (EfiSys)) == 0) {
          *EfiPart = Handles[I];
        }
        if (Hd->SignatureType == SIGNATURE_TYPE_GUID && CompareMem (Hd->Signature, &SsdBoot, sizeof (SsdBoot)) == 0) {
          *SsdPart = Handles[I];
          ByGuid = TRUE;
        }
      }
#endif
    }
    if (ByGuid) {
      ++Boots;
    } else if (Usb) {
      EFI_PARTITION_INFO_PROTOCOL *Pi;
      if (!EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiPartitionInfoProtocolGuid, (VOID **)&Pi)) &&
          Pi->Type == PARTITION_TYPE_GPT && StrnCmp (Pi->Info.Gpt.PartitionName, L"PS5BOOT", 8) == 0 &&
          WindowsFileExists (Handles[I], L"\\ps5win.id")) {
        *SsdPart = Handles[I];
        ++Boots;
      }
    }
  }
  FreePool (Handles);
  if (Boots > 1) {
    Print (L"Two PS5 Windows boot partitions (PS5BOOT) are attached. Unplug one disk. Stopped.\r\n");
    CpuDeadLoop ();
  }
}

// Boot only the approved USB GPT partition. The BDS fallback scans other disks,
// so every failure here stays on screen until power-off.
// Warm restart, experiment 3a (firmware-only loop, no Windows): only when stage 1 set the test flag (launch option
// ps5warm=3a). Pass 1 arms the loop, ends boot services and asks for a warm reset, as an OS would; the reset library's
// stub re-enters stage 1, which starts this firmware again. Pass 2 says so on screen, disarms and goes on to Windows.
// Record layout: firmware/ps5warm/Ps5Warm.h (REC_* offsets, BLK_ARMED = 0x80).
#define WARM_REC        0x3000F0000ULL
#define WARM_REC32(o)   (*(volatile UINT32 *)(UINTN)(WARM_REC + (o)))
#define WARM_REC64(o)   (*(volatile UINT64 *)(UINTN)(WARM_REC + (o)))
STATIC VOID WarmShowBox (UINT8 R, UINT8 G, UINT8 B)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)) && Gop != NULL) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL C = { B, G, R, 0 };
    Gop->Blt (Gop, &C, EfiBltVideoFill, 0, 0, 40, 120, 600, 200, 0);
  }
}
STATIC VOID WarmTest3a (VOID)
{
  UINT64 Blk;
  UINT32 Loops;
  if (!PS5_WARM_ENABLED) return;
  if (WARM_REC64 (0x00) != 0x3143455257355350ULL || (WARM_REC32 (0x2C) & 1) == 0) return;
  Loops = WARM_REC32 (0x44);
  Blk = WARM_REC64 (0x50);
  if (Loops >= 1) {
    WARM_REC32 (0x40) = 0;                               // disarm: Windows starts unarmed after the test
    WARM_REC32 (0x2C) &= ~1U;                            // the test is over: later warm runs (Windows' Restart) keep their arming
    if (Blk != 0) *(volatile UINT32 *)(UINTN)(Blk + 0x80) = 0;
    WarmShowBox (0, 200, 0);
    Print (L"\r\n==== WARM RESTART TEST 3a: PASS 2 (loop %u, last stub step %u). The firmware started again without a power\r\n"
           L"==== cycle and found the USB disk. Disarmed; going on to Windows in 15 s.\r\n", Loops, WARM_REC32 (0x58));
    gBS->Stall (15000000);
    return;
  }
  if (Blk == 0) {
    WarmShowBox (200, 0, 0);
    Print (L"\r\n==== WARM RESTART TEST 3a: no warm-restart block (record or firmware check failed). Skipped; going on to Windows.\r\n");
    gBS->Stall (15000000);
    return;
  }
  WarmShowBox (0, 0, 200);
  Print (L"\r\n==== WARM RESTART TEST 3a: pass 1. Arming, ending boot services, then a warm reset in 10 s.\r\n");
  gBS->Stall (10000000);
  *(volatile UINT32 *)(UINTN)(Blk + 0x80) = 1;
  {
    STATIC UINT8 Map[0x10000];
    UINTN Try;
    for (Try = 0; Try < 3; Try++) {
      UINTN Size = sizeof (Map), Key = 0, DescSize = 0;
      UINT32 Ver = 0;
      if (EFI_ERROR (gBS->GetMemoryMap (&Size, (EFI_MEMORY_DESCRIPTOR *)Map, &Key, &DescSize, &Ver))) continue;
      if (!EFI_ERROR (gBS->ExitBootServices (gImageHandle, Key))) {
        gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);
        CpuDeadLoop ();                                  // the ICC restart would have ended the run instead
      }
    }
  }
  *(volatile UINT32 *)(UINTN)(Blk + 0x80) = 0;
  Print (L"==== WARM RESTART TEST 3a: ExitBootServices failed. Disarmed; going on to Windows.\r\n");
}

// Warm restart: what stage 1 found in the PCI state Windows left, and what it did about it (firmware/ps5warm/warm_pci.h,
// report at record + 0x800: magic "WPD1", stage run, xHCI count, bus 0x20 D-states, two root-port entries of 0x20 at
// +0x10, four xHCI entries of 0x30 at +0x50). Shown on warm runs only, under the USB search result, so it stays on
// screen when the boot stops there. Configuration reads only, and only of the functions named in the report.
STATIC UINT32 WarmCfg (UINT32 Bdf, UINT32 Off)
{
  IoWrite32 (0xCF8, 0x80000000U | (Bdf << 8) | (Off & 0xFC));
  return IoRead32 (0xCFC);
}
STATIC VOID WarmPciReport (VOID)
{
  UINT32 I, N;
  if (!PS5_WARM_ENABLED) return;
  if (WARM_REC64 (0x00) != 0x3143455257355350ULL || WARM_REC32 (0x4C) < 2 || WARM_REC32 (0x800) != 0x31445057U) return;
  N = WARM_REC32 (0x808);
  Print (L"Warm restart, loop %u (stage run %u): PCI state as Windows left it -> what stage 1 did. Bus 20 D-states %08x.\r\n",
         WARM_REC32 (0x44), WARM_REC32 (0x804), WARM_REC32 (0x80C));
  for (I = 0; I < 2; I++) {
    UINT32 O = 0x810 + I * 0x20, Bdf = WARM_REC32 (O);
    if (Bdf == 0) continue;
    Print (L"  root port %02x:%02x.%x found cmd %04x buses %06x window %08x pm %04x; flags %03x%s; now cmd %04x buses %06x\r\n",
           Bdf >> 8, (Bdf >> 3) & 31, Bdf & 7, WARM_REC32 (O + 4) & 0xFFFF, WARM_REC32 (O + 8) & 0xFFFFFF, WARM_REC32 (O + 12),
           WARM_REC32 (O + 16) & 0xFFFF, WARM_REC32 (O + 20), (WARM_REC32 (O + 20) & 8) ? L" (restored)" : L"",
           WarmCfg (Bdf, 4) & 0xFFFF, WarmCfg (Bdf, 0x18) & 0xFFFFFF);
  }
  for (I = 0; I < N && I < 4; I++) {
    UINT32 O = 0x850 + I * 0x30, Bdf = WARM_REC32 (O), F = WARM_REC32 (O + 16);
    Print (L"  xhci %02x:%02x.%x found cmd %04x pm %04x bar %08x; flags %03x:%s%s%s%s%s%s %s (%u us), usbsts %08x\r\n",
           Bdf >> 8, (Bdf >> 3) & 31, Bdf & 7, WARM_REC32 (O + 4) & 0xFFFF, WARM_REC32 (O + 8) & 0xFFFF, WARM_REC32 (O + 12), F,
           (F & 2) ? L" was asleep," : L" was in D0,", (F & 4) ? L"" : L" NOT D0 NOW,", (F & 8) ? L" header restored," : L"",
           (F & 0x10) ? L" vendor bytes restored," : L"", (F & 0x400) ? L" power-cycled," : L"", (F & 0x20) ? L"" : L" REGISTERS DEAD,",
           (F & 0x80) ? L"reset ok" : (F & 0x800) ? L"halted, not reset" : L"reset FAILED", WARM_REC32 (O + 24), WARM_REC32 (O + 20));
    if ((F & 0x1000) != 0) {
      Print (L"       disk re-plugged by port power: %s while off, %s", (F & 0x2000) ? L"gone" : L"STAYED CONNECTED",
             (F & 0x4000) ? L"back after " : L"NOT BACK\r\n");
      if ((F & 0x4000) != 0) Print (L"%u ms\r\n", WARM_REC32 (O + 24) / 1000);
    }
    Print (L"       %u ports after stage 1: %08x %08x %08x %08x (bit 0 = connected)\r\n",
           WARM_REC32 (O + 28), WARM_REC32 (O + 32), WARM_REC32 (O + 36), WARM_REC32 (O + 40), WARM_REC32 (O + 44));
    {
      // The same function right now (read only): is it decoded, running, and does a port see the disk?
      UINT32 Cmd = WarmCfg (Bdf, 4) & 0xFFFF, Bar = WarmCfg (Bdf, 0x10);
      volatile UINT8 *B = (volatile UINT8 *)(UINTN)(Bar & 0xFFFFFFF0U);
      if ((Cmd & 2) != 0 && (Bar & 0xFFFFFFF0U) != 0 && (Bar & 1) == 0 && WarmCfg (Bdf, 0x14) == 0 &&
          *(volatile UINT32 *)B != 0xFFFFFFFFU && B[0] >= 0x10) {
        volatile UINT32 *Op = (volatile UINT32 *)(B + B[0]);
        Print (L"       now: cmd %04x bar %08x usbcmd %08x usbsts %08x ports %08x %08x %08x %08x\r\n", Cmd, Bar, Op[0], Op[1],
               Op[0x100], Op[0x104], Op[0x108], Op[0x10C]);
      } else {
        Print (L"       now: cmd %04x bar %08x, registers not readable\r\n", Cmd, Bar);
      }
    }
  }
}

// Warm restart evidence that needs no screen and survives a power cut (2026-11-24, after the boot49w Restart that left
// nothing to read): every run that finds the boot partition appends its identity to the boot log \ps5-boot22.txt
// (append-only, never truncated) as its FIRST disk write, and writes the same lines into the trace. "stage run 1" came
// from Linux; a higher number came from a Restart without a power cycle, and then stage 1's PCI report follows (what
// Windows left, what was restored). Before this a warm run looked exactly like a launch in both files.
STATIC CHAR8 mWarmText[3072];
STATIC UINTN mWarmTextLen;
STATIC VOID WarmEvidence (EFI_HANDLE Target, UINT32 HalfSeconds)
{
  UINTN  N = 0;
  UINT32 I, Run;
  mWarmTextLen = 0;
  if (!PS5_WARM_ENABLED || WARM_REC64 (0x00) != 0x3143455257355350ULL) return;
  Run = WARM_REC32 (0x4C);
  N += AsciiSPrint (mWarmText + N, sizeof (mWarmText) - N,
         "Warm: stage run %u (%a%a), loop %u, armed %u, flags %x, last stub step %x; boot partition found after %u.%u s.\r\n",
         Run, Run < 2 ? "launch from Linux" : (WARM_REC32 (0x2C) & 1) ? "firmware-only loop" : "Restart without a power cycle",
         (WARM_REC32 (0x40) & 0x100) ? ", handed to the boot CPU from another CPU" : "", WARM_REC32 (0x44), WARM_REC32 (0x40) & 0xFF,
         WARM_REC32 (0x2C), WARM_REC32 (0x58), HalfSeconds / 2, HalfSeconds % 2 * 5);
  if (Run >= 2 && WARM_REC32 (0x800) == 0x31445057U) {
    N += AsciiSPrint (mWarmText + N, sizeof (mWarmText) - N, "Warm: stage 1 report of run %u: bus 20 D-states %08x.\r\n",
           WARM_REC32 (0x804), WARM_REC32 (0x80C));
    for (I = 0; I < 2 && N + 200 < sizeof (mWarmText); I++) {
      UINT32 O = 0x810 + I * 0x20, Bdf = WARM_REC32 (O);
      if (Bdf == 0) continue;
      N += AsciiSPrint (mWarmText + N, sizeof (mWarmText) - N,
             "Warm: root port %02x:%02x.%x found cmd %04x buses %06x window %08x pm %04x flags %03x.\r\n",
             Bdf >> 8, (Bdf >> 3) & 31, Bdf & 7, WARM_REC32 (O + 4) & 0xFFFF, WARM_REC32 (O + 8) & 0xFFFFFF, WARM_REC32 (O + 12),
             WARM_REC32 (O + 16) & 0xFFFF, WARM_REC32 (O + 20));
    }
    for (I = 0; I < WARM_REC32 (0x808) && I < 4 && N + 260 < sizeof (mWarmText); I++) {
      UINT32 O = 0x850 + I * 0x30, Bdf = WARM_REC32 (O);
      N += AsciiSPrint (mWarmText + N, sizeof (mWarmText) - N,
             "Warm: xhci %02x:%02x.%x found cmd %04x pm %04x bar %08x flags %03x reset %u us usbsts %08x ports(%u) %08x %08x %08x %08x.\r\n",
             Bdf >> 8, (Bdf >> 3) & 31, Bdf & 7, WARM_REC32 (O + 4) & 0xFFFF, WARM_REC32 (O + 8) & 0xFFFF, WARM_REC32 (O + 12),
             WARM_REC32 (O + 16), WARM_REC32 (O + 24), WARM_REC32 (O + 20), WARM_REC32 (O + 28), WARM_REC32 (O + 32),
             WARM_REC32 (O + 36), WARM_REC32 (O + 40), WARM_REC32 (O + 44));
    }
  }
  // Every run, launches from Linux included (so a good and a bad start can be compared): each xHCI of stage 1's snapshot
  // as it is right now, after this firmware's own driver has reset it and found the disk. PORTSC bit 0 = connected,
  // bit 1 = enabled, bits 13:10 = speed (4 = 5 Gbit/s, 3 = 480 Mbit/s). Reads only.
  if (WARM_REC32 (0x100) == 0x31475057U && WARM_REC32 (0x104) <= 6) {
    for (I = 0; I < WARM_REC32 (0x104) && N + 200 < sizeof (mWarmText); I++) {
      UINT32 E = 0x110 + I * 0x110, Bdf = WARM_REC32 (E), Cmd, Bar;
      volatile UINT8 *B;
      if (WARM_REC32 (E + 8) != 2) continue;
      Cmd = WarmCfg (Bdf, 4) & 0xFFFF;
      Bar = WarmCfg (Bdf, 0x10);
      B   = (volatile UINT8 *)(UINTN)(Bar & 0xFFFFFFF0U);
      if ((Cmd & 2) != 0 && (Bar & 0xFFFFFFF0U) != 0 && (Bar & 1) == 0 && WarmCfg (Bdf, 0x14) == 0 &&
          *(volatile UINT32 *)B != 0xFFFFFFFFU && B[0] >= 0x10) {
        volatile UINT32 *Op = (volatile UINT32 *)(B + B[0]);
        N += AsciiSPrint (mWarmText + N, sizeof (mWarmText) - N,
               "Warm: now xhci %02x:%02x.%x cmd %04x bar %08x usbcmd %08x usbsts %08x ports %08x %08x %08x %08x.\r\n",
               Bdf >> 8, (Bdf >> 3) & 31, Bdf & 7, Cmd, Bar, Op[0], Op[1], Op[0x100], Op[0x104], Op[0x108], Op[0x10C]);
      } else {
        N += AsciiSPrint (mWarmText + N, sizeof (mWarmText) - N, "Warm: now xhci %02x:%02x.%x cmd %04x bar %08x, registers not readable.\r\n",
               Bdf >> 8, (Bdf >> 3) & 31, Bdf & 7, Cmd, Bar);
      }
    }
  }
  mWarmTextLen = N;
  BootMark (Target, mWarmText);                           // best effort, like every boot log line
}

// A start that came from a Restart without a power cycle and cannot start Windows (no boot partition, or the boot
// manager came back) must not sit here until someone holds the power button: after two minutes for reading the screen
// it restarts the console the normal way (through the console's own system), as an unarmed Restart does. Without a
// screen that outcome also says how far the run got: a normal restart by itself = the firmware ran but Windows could
// not be started; none = stopped elsewhere.
STATIC VOID WarmFallback (VOID)
{
  STATIC UINT8 Map[0x10000];
  UINT64 Blk;
  UINT32 S;
  UINTN  Try;
  if (!PS5_WARM_ENABLED || WARM_REC64 (0x00) != 0x3143455257355350ULL || WARM_REC32 (0x4C) < 2) return;
  Print (L"\r\nThis start came from a Restart without a power cycle (stage run %u) and Windows could not be started.\r\n"
         L"The console restarts in 120 s; then start the loader again. Hold the power button to turn it off instead.\r\n",
         WARM_REC32 (0x4C));
  for (S = 120; S > 0; S -= 10) {
    Print (L" %u", S);
    gBS->Stall (10000000);
  }
  WARM_REC32 (0x40) = 0;                                   // disarm: the reset below must go to the console's controller
  Blk = WARM_REC64 (0x50);
  if (Blk != 0) *(volatile UINT32 *)(UINTN)(Blk + 0x80) = 0;
  TraceFlush (TRUE);
  mExited = TRUE;                                          // no trace writes from here on
  if (mOrigReset != NULL) gRT->ResetSystem = mOrigReset;   // the real services, not the trace hooks (TraceInstall may have run)
  for (Try = 0; Try < 3; Try++) {
    UINTN Size = sizeof (Map), Key = 0, DescSize = 0;
    UINT32 Ver = 0;
    if (EFI_ERROR ((mOrigMap != NULL ? mOrigMap : gBS->GetMemoryMap) (&Size, (EFI_MEMORY_DESCRIPTOR *)Map, &Key, &DescSize, &Ver))) continue;
    if (!EFI_ERROR ((mOrigEbs != NULL ? mOrigEbs : gBS->ExitBootServices) (gImageHandle, Key))) {
      gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
      CpuDeadLoop ();
    }
  }
  Print (L"\r\nCould not end boot services for the restart. Hold the power button to turn the console off.\r\n");
}

STATIC VOID WindowsBoot (VOID)
{
  EFI_HANDLE Target = NULL, EfiPart = NULL, SsdPart = NULL;
  EFI_STATUS Status;
  BOOLEAN SsdMode = FALSE;
  UINTN Try;
  // Boot 28: the USB ports are examined from a timer after the controllers start, so a disk can appear seconds after
  // EfiBootManagerConnectAll has returned (boot27, the first launch without the Toshiba, searched once and found
  // nothing). Search every half second: until the SSD's boot partition is there; with only the Toshiba there, 5 s
  // (the SSD wins when both are plugged in); with nothing, 30 s.
  for (Try = 0; ; ++Try) {
    WindowsFindVolumes (&Target, &EfiPart, &SsdPart);
    SsdMode = SsdPart != NULL && WindowsFileExists (SsdPart, L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
    if (SsdMode || (Target != NULL && Try >= 10) || Try >= 60) break;
    if (Try == 0) Print (L"Boot 28: waiting for the USB disks (up to 30 s).\r\n");
    gBS->Stall (500000);
    EfiBootManagerConnectAll ();
  }
  Print (L"Boot 28: after %u.%u s: %u file systems (%u on USB), %u USB disks and partitions, %u USB interfaces.\r\n",
         (UINT32)(Try / 2), (UINT32)(Try % 2 * 5),
         (UINT32)WindowsCountHandles (&gEfiSimpleFileSystemProtocolGuid, FALSE),
         (UINT32)WindowsCountHandles (&gEfiSimpleFileSystemProtocolGuid, TRUE),
         (UINT32)WindowsCountHandles (&gEfiBlockIoProtocolGuid, TRUE),
         (UINT32)WindowsCountHandles (&gEfiUsbIoProtocolGuid, FALSE));
  Print (L"Boot 28: Windows boot partition %s, boot manager on it %s, installer disk %s.\r\n",
         SsdPart != NULL ? L"found" : L"NOT found", SsdMode ? L"found" : L"NOT found",
         Target != NULL ? L"found" : L"not there");
  WarmPciReport ();                                      // warm runs only: the PCI state Windows left and what stage 1 did
  if (SsdMode) {
    Target = SsdPart;
    EfiPart = SsdPart;
  }
  if (Target != NULL) {
    EFI_BOOT_MANAGER_LOAD_OPTION Option;
    CHAR8 Message[128];
    WarmEvidence (Target, (UINT32)Try);                  // first disk write of the run: which run this is (boot log)
    if (!WindowsMapSafe () || !WindowsAcpiSafe ()) {
      BootMark (Target, "Stopped: memory map or ACPI verification failed.\r\n");
      Print (L"Stopped: Windows memory or PCI map check failed.\r\n");
      CpuDeadLoop ();
    }
    WarmTest3a ();                                       // warm restart experiment 3a (test flag only)
    // Boot 22: the marker \ps5-boot-installed.txt on the installer volume (written by the WinPE install script, removable from
    // Linux) selects the installed Windows on the EFI partition. Without it, or without boot files there, WinPE starts as before.
    BOOLEAN Installed = SsdMode || (WindowsFileExists (Target, L"\\ps5-boot-installed.txt") &&
                        WindowsFileExists (EfiPart, L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi"));
    EFI_HANDLE BootVolume = Installed ? EfiPart : Target;
    Print (L"Boot 26: firmware is checking the %s (%s).\r\n", SsdMode ? L"Windows disk" : L"installer disk", Installed ? L"installed Windows" : L"WinPE installer");
    // 2026-11-22: the boot log line is best effort; it must never decide whether Windows starts (a full or read-only
    // PS5BOOT used to stop the boot here).
    BootMark (Target, Installed ? "Boot 22: firmware checks passed. Marker found: starting the installed Windows from the EFI partition.\r\n"
                                : "Boot 22: firmware checks passed. Verifying media.\r\n");
    Status = EFI_SUCCESS;
    if (!EFI_ERROR (Status) && !Installed) Status = VerifyMedia (Target);
    if (!EFI_ERROR (Status)) {
      EFI_DEVICE_PATH_PROTOCOL *Path = FileDevicePath (BootVolume, L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
      Status = Path == NULL ? EFI_OUT_OF_RESOURCES : EfiBootManagerInitializeLoadOption (
        &Option, LoadOptionNumberUnassigned, LoadOptionTypeBoot, LOAD_OPTION_ACTIVE,
        SsdMode ? L"Windows (PS5BOOT)" : Installed ? L"Windows on the installer disk" : L"Windows PE on the installer disk", Path, NULL, 0);
      if (Path != NULL) FreePool (Path);
      if (!EFI_ERROR (Status)) {
        EFI_STATUS TraceStatus = TraceOpen (Target);
        AsciiSPrint (Message, sizeof (Message), "Boot 22: trace file %r. Launching Windows via the UEFI boot option.\r\n", TraceStatus);
        BootMark (Target, Message);    // best effort (see above)
        {
          Print (L"Media verified. Starting Windows Boot Manager (trace %r).\r\n", TraceStatus);
          // 2026-11-22: ALWAYS. TraceInstall also clears EFER bit 16, prepares reset/shutdown and hooks ExitBootServices
          // (HAL I/O APIC patch, kernel stub). It used to run only when a trace file could be created, so once all 9
          // trace slots on PS5BOOT were used, Windows started without the PS5 fixes and hung at the logo (Nov 20-22).
          TraceInstall ();
          if (mWarmTextLen != 0) {                         // the same identity lines in the trace (slot 9 is reused by every run)
            TraceEvent ("Warm restart state of this run: ");
            TraceRaw ("\r\n", 2);
            TraceRaw (mWarmText, mWarmTextLen);
            TraceFlush (TRUE);
          }
          // Boot 29: hand Windows a clean, black screen. Windows draws only its logo and dots over the picture it is
          // given, so stage 1's text, this firmware's log ring and its marks stayed visible under the logo. The log
          // ring stops drawing (flag read by Ps5FbSerialPortLib) and the whole picture is filled with black; the trace
          // file keeps everything.
          *(volatile UINT32 *)(UINTN)0x300001f18ULL = 1;
          {
            EFI_GRAPHICS_OUTPUT_PROTOCOL *CleanGop = NULL;
            if (!EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&CleanGop)) && CleanGop != NULL) {
              EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black = { 0, 0, 0, 0 };
              EFI_STATUS FillStatus = CleanGop->Blt (CleanGop, &Black, EfiBltVideoFill, 0, 0, 0, 0, CleanGop->Mode->Info->HorizontalResolution,
                                                     CleanGop->Mode->Info->VerticalResolution, 0);
              TraceEvent ("Boot 29: screen cleared to black for Windows (%r) ", FillStatus);
            }
          }
          EfiBootManagerBoot (&Option);
          Status = Option.Status;
          TraceEvent ("Boot option returned %r ", Status);
          TraceFlush (TRUE);
        }
        EfiBootManagerFreeLoadOption (&Option);
      }
    }
    AsciiSPrint (Message, sizeof (Message), "Boot 22: boot option stopped, %r (0x%Lx).\r\n", Status, (UINT64)Status);
    BootMark (Target, Message);
  } else {
    Status = EFI_NOT_FOUND;
  }
  Print (L"Windows boot stopped: %r\r\n", Status);
  WarmFallback ();                                       // warm runs only: a normal restart after two minutes instead of a dead end
  CpuDeadLoop ();
}
