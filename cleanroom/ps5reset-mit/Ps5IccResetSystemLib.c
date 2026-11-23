// SPDX-License-Identifier: MIT
//
// ResetSystemLib for the PS5: power off / restart through the southbridge (EMC) ICC mailbox.
// Written from windows-handoff/cleanroom/spec-icc-reset.md only. See LICENSE in this folder.
//
// Copyright (c) 2026 ps5win contributors
//
// Before ExitBootServices every reset request halts (CpuDeadLoop). After it the power messages are sent;
// any failure halts. Nothing is ever sent to the PSP or SMU.

#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/ResetSystemLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>

#include "Ps5Icc.h"
#include "Ps5Warm.h"                 // warm restart (firmware/ps5warm, not clean-room: our own design)

#define PS5_GENERAL_TIMEOUT_MS  2000
#define PS5_POWER_TIMEOUT_MS    5000
#define PS5_POWER_WAIT_MS       10000

STATIC BOOLEAN    mPs5AfterExitBootServices = FALSE;
STATIC EFI_EVENT  mPs5ExitBootServicesEvent = NULL;
STATIC EFI_EVENT  mPs5VirtualAddressEvent   = NULL;

STATIC
VOID
Ps5ConvertMmio (
  IN OUT volatile UINT8  **Pointer
  )
{
  VOID  *Address;

  if (*Pointer == NULL) {
    return;
  }

  Address = (VOID *)(UINTN)*Pointer;
  if (EFI_ERROR (EfiConvertPointer (0, &Address))) {
    Address = NULL;                       // page not described as runtime: never touch it afterwards
  }

  *Pointer = (volatile UINT8 *)Address;
}

STATIC
VOID
EFIAPI
Ps5OnVirtualAddressChange (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  Ps5ConvertMmio (&mPs5Doorbell);
  Ps5ConvertMmio (&mPs5Mailbox);
  Ps5VariablesConvert ();
  Ps5WarmConvert ();
}

STATIC
VOID
EFIAPI
Ps5OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  mPs5AfterExitBootServices = TRUE;
  Ps5VariablesInstall ();
}

/**
  Ask the controller to cut (PS5_POWER_OFF) or reset (PS5_POWER_RESTART) power. Never returns.
**/
STATIC
VOID
Ps5Power (
  IN UINT8  Action
  )
{
  STATIC CONST UINT8  General0[2] = { 0, 0 };
  STATIC CONST UINT8  General1[2] = { 1, 0 };
  UINT8               PowerData[5];

  Ps5WarmMarkDeclined ();                     // diagnostic: red square 15 when a test run ends up here
  if (!mPs5AfterExitBootServices) {
    CpuDeadLoop ();
  }

  if (!Ps5BridgeEnsure (NULL, NULL) || !Ps5MailboxSane ()) {
    CpuDeadLoop ();
  }

  Ps5MailboxMaskOff ();

  //
  // Service 2 type 0x10 twice; if the first gets no reply go straight to the power message.
  //
  if (Ps5IccTransact (ICC_SVC_GENERAL, ICC_GENERAL_T10, General0, sizeof (General0),
        PS5_GENERAL_TIMEOUT_MS, NULL, 0, NULL))
  {
    Ps5IccTransact (ICC_SVC_GENERAL, ICC_GENERAL_T10, General1, sizeof (General1),
      PS5_GENERAL_TIMEOUT_MS, NULL, 0, NULL);
  }

  PowerData[0] = 0;
  PowerData[1] = Action;
  PowerData[2] = 2;
  PowerData[3] = 0;
  PowerData[4] = 1;
  //
  // Sent even if the general messages got no reply. Wait whether or not it is answered; the controller
  // cuts or resets power meanwhile.
  //
  Ps5IccTransact (ICC_SVC_POWER, ICC_POWER_T1, PowerData, sizeof (PowerData), PS5_POWER_TIMEOUT_MS, NULL, 0, NULL);
  Ps5DelayMs (PS5_POWER_WAIT_MS);

  CpuDeadLoop ();
}

VOID
EFIAPI
ResetCold (
  VOID
  )
{
  Ps5WarmTry (mPs5AfterExitBootServices);   // returns unless armed and every check passes
  Ps5Power (PS5_POWER_RESTART);
}

VOID
EFIAPI
ResetWarm (
  VOID
  )
{
  Ps5WarmTry (mPs5AfterExitBootServices);   // Windows' Restart arrives here (EfiResetWarm)
  Ps5Power (PS5_POWER_RESTART);
}

VOID
EFIAPI
ResetShutdown (
  VOID
  )
{
  Ps5Power (PS5_POWER_OFF);
}

VOID
EFIAPI
ResetPlatformSpecific (
  IN UINTN  DataSize,
  IN VOID   *ResetData
  )
{
  Ps5Power (PS5_POWER_RESTART);
}

VOID
EFIAPI
ResetSystem (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  if (ResetType == EfiResetShutdown) {
    ResetShutdown ();
  } else {
    ResetCold ();
  }
}

/**
  Library constructor: save the root port state while it is still the one the firmware set up, and hook
  ExitBootServices / SetVirtualAddressMap.
**/
EFI_STATUS
EFIAPI
Ps5IccResetSystemLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Ps5BridgeSaveAtBoot ();
  Ps5WarmInit ();

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  Ps5OnExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mPs5ExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_NOTIFY,
                Ps5OnVirtualAddressChange,
                NULL,
                &gEfiEventVirtualAddressChangeGuid,
                &mPs5VirtualAddressEvent
                );
}
