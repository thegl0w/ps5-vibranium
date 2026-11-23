// SPDX-License-Identifier: MIT
#include "Ps5WarmConfig.h"     // PS5_WARM_ENABLED, PS5_DEV_DISKS (written by firmware/prepare.py)
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
        (Lo < 0x470000000ULL && Hi > 0x370000000ULL)) {
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
  if (SsdMode) {
    Target = SsdPart;
    EfiPart = SsdPart;
  }
  if (Target != NULL) {
    EFI_BOOT_MANAGER_LOAD_OPTION Option;
    CHAR8 Message[128];
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
  CpuDeadLoop ();
}
