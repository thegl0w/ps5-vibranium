// SPDX-License-Identifier: MIT
//
// "Ps5Thermal" (GetVariable) and "Ps5FanMode" (SetVariable) runtime interface for the PS5.
// Written from windows-handoff/cleanroom/spec-icc-reset.md only. See LICENSE in this folder.
//
// Copyright (c) 2026 ps5win contributors
//
// Installed at ExitBootServices by replacing gRT->GetVariable / gRT->SetVariable; every other variable goes
// to the original services. All work happens on the caller's thread inside the runtime call.

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/SynchronizationLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "Ps5Icc.h"
#include "Ps5Warm.h"

STATIC EFI_GUID          mPs5VarGuid = PS5_VAR_GUID;
STATIC CONST CHAR16      mPs5ThermalName[] = L"Ps5Thermal";
STATIC CONST CHAR16      mPs5FanModeName[] = L"Ps5FanMode";
STATIC CONST CHAR16      mPs5WarmName[]    = L"Ps5WarmRestart";   // warm restart: Set 1 byte 0/1, Get 16 bytes {valid, armed, loops, cap};
                                                                // a buffer of 16 + PS5_WARM_DIAG_SIZE also gets stage 1's PCI restore report

STATIC EFI_GET_VARIABLE  mPs5OrigGetVariable = NULL;
STATIC EFI_SET_VARIABLE  mPs5OrigSetVariable = NULL;
STATIC BOOLEAN           mPs5VarsInstalled   = FALSE;

STATIC volatile UINT32   mPs5Busy     = 0;
STATIC UINT32            mPs5Calls    = 0;
STATIC UINT32            mPs5SetCalls = 0;

STATIC CONST UINT8       mPs5Sensors[5] = { 0, 1, 2, 3, 18 };

#define PS5_TEMP_LIMIT   (125 * 256)
#define PS5_DUTY_MAX     1024

STATIC
BOOLEAN
Ps5NameIs (
  IN CONST CHAR16  *Name,
  IN CONST CHAR16  *Want
  )
{
  while ((*Name != 0) && (*Name == *Want)) {
    Name++;
    Want++;
  }

  return (BOOLEAN)(*Name == *Want);
}

STATIC
BOOLEAN
Ps5IsOurVariable (
  IN CONST CHAR16    *Name,
  IN CONST EFI_GUID  *Guid,
  IN CONST CHAR16    *Want
  )
{
  return (BOOLEAN)((Name != NULL) && (Guid != NULL) && CompareGuid (Guid, &mPs5VarGuid) && Ps5NameIs (Name, Want));
}

STATIC
BOOLEAN
Ps5TryLock (
  VOID
  )
{
  return (BOOLEAN)(InterlockedCompareExchange32 ((volatile UINT32 *)&mPs5Busy, 0, 1) == 0);
}

STATIC
VOID
Ps5Unlock (
  VOID
  )
{
  MemoryFence ();
  mPs5Busy = 0;
}

STATIC
INT32
Ps5ReadTemp (
  IN UINT8  Sensor
  )
{
  UINT8   Req[4];
  UINT16  Raw;
  INT32   Result;
  INT32   Value;

  Req[0] = Sensor;
  Req[1] = 0;
  Req[2] = 0;
  Req[3] = 0;
  Result = Ps5IccQueryValue (ICC_SVC_THERMAL, ICC_THERMAL_READ, Req, sizeof (Req), &Raw);
  if (Result < 0) {
    return Result;
  }

  Value = (INT16)Raw;                       // degrees C * 256, signed
  if ((Value < 0) || (Value >= PS5_TEMP_LIMIT)) {
    return PS5_ERR_RANGE;
  }

  return (Value * 1000 + 128) / 256;
}

STATIC
VOID
Ps5ReadThermal (
  OUT PS5_THERMAL  *T
  )
{
  STATIC CONST UINT8  ModeReq[4] = { 0, 0, 0, 0 };
  UINT32              Found[RP_NUM_REGS];
  BOOLEAN             Restored;
  BOOLEAN             BridgeOk;
  UINTN               Index;
  UINT16              Raw;
  INT32               Result;

  ZeroMem (T, sizeof (*T));
  T->Version  = PS5_THERMAL_VERSION;
  T->Calls    = mPs5Calls;
  T->SetCalls = mPs5SetCalls;
  if (mPs5BootBridge.Valid) {
    T->Flags |= PS5_THERMAL_F_BOOT_STATE;
  }

  for (Index = 0; Index < ARRAY_SIZE (T->TempMilliC); Index++) {
    T->TempMilliC[Index] = PS5_ERR_NO_REPLY;
  }

  T->Duty = PS5_ERR_NO_REPLY;
  T->Mode = PS5_ERR_NO_REPLY;

  BridgeOk     = Ps5BridgeEnsure (Found, &Restored);
  T->Bridge[0] = Found[0];                  // 0x04
  T->Bridge[1] = Found[1];                  // 0x18
  T->Bridge[2] = Found[3];                  // 0x20
  T->Bridge[3] = Found[4];                  // 0x24
  if (Restored) {
    T->Flags |= PS5_THERMAL_F_RESTORED;
  }

  if (BridgeOk) {
    T->Flags |= PS5_THERMAL_F_BRIDGE_OK;
    if (Ps5MailboxSane ()) {
      T->Flags |= PS5_THERMAL_F_MAILBOX_OK;
      Ps5MailboxMaskOff ();

      for (Index = 0; Index < ARRAY_SIZE (mPs5Sensors); Index++) {
        T->TempMilliC[Index] = Ps5ReadTemp (mPs5Sensors[Index]);
      }

      Result = Ps5IccQueryValue (ICC_SVC_FAN, ICC_FAN_DUTY, NULL, 0, &Raw);
      if (Result == 0) {
        Result = (Raw <= PS5_DUTY_MAX) ? (INT32)Raw : PS5_ERR_BAD_LEN;   // -2, as before
      }

      T->Duty = Result;

      Result = Ps5IccQueryValue (ICC_SVC_FAN, ICC_FAN_MODE_GET, ModeReq, sizeof (ModeReq), &Raw);
      T->Mode = (Result == 0) ? (INT32)(Raw & 0xFF) : Result;
    }
  }

  if (Restored) {
    Ps5BridgePutBack (Found);
  }
}

STATIC
EFI_STATUS
Ps5SetFanMode (
  IN UINT8  Mode
  )
{
  UINT32      Found[RP_NUM_REGS];
  BOOLEAN     Restored;
  UINT8       Req[4];
  UINT16      Raw;
  EFI_STATUS  Status;

  Status = EFI_DEVICE_ERROR;
  if (Ps5BridgeEnsure (Found, &Restored) && Ps5MailboxSane ()) {
    Ps5MailboxMaskOff ();
    Req[0] = 0;
    Req[1] = Mode;
    Req[2] = 0;
    Req[3] = 0;
    if (Ps5IccQueryValue (ICC_SVC_FAN, ICC_FAN_MODE_SET, Req, sizeof (Req), &Raw) == 0) {
      Status = EFI_SUCCESS;
    }
  }

  if (Restored) {
    Ps5BridgePutBack (Found);
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Ps5GetVariable (
  IN     CHAR16    *VariableName,
  IN     EFI_GUID  *VendorGuid,
  OUT    UINT32    *Attributes     OPTIONAL,
  IN OUT UINTN     *DataSize,
  OUT    VOID      *Data           OPTIONAL
  )
{
  PS5_THERMAL  Thermal;
  UINT32       Warm[4];
  STATIC UINT8 WarmLong[sizeof (Warm) + PS5_WARM_DIAG_SIZE];

  if (Ps5IsOurVariable (VariableName, VendorGuid, mPs5WarmName)) {
    if (DataSize == NULL) {
      return EFI_INVALID_PARAMETER;
    }

    if (*DataSize < sizeof (Warm)) {
      *DataSize = sizeof (Warm);
      return EFI_BUFFER_TOO_SMALL;
    }

    if (Data == NULL) {
      return EFI_INVALID_PARAMETER;
    }

    Ps5WarmStatus (Warm);
    if (*DataSize >= sizeof (WarmLong)) {
      CopyMem (WarmLong, Warm, sizeof (Warm));
      Ps5WarmDiag (WarmLong + sizeof (Warm));
      CopyMem (Data, WarmLong, sizeof (WarmLong));
      *DataSize = sizeof (WarmLong);
    } else {
      CopyMem (Data, Warm, sizeof (Warm));
      *DataSize = sizeof (Warm);
    }

    if (Attributes != NULL) {
      *Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
    }

    return EFI_SUCCESS;
  }

  if (!Ps5IsOurVariable (VariableName, VendorGuid, mPs5ThermalName)) {
    return mPs5OrigGetVariable (VariableName, VendorGuid, Attributes, DataSize, Data);
  }

  if (DataSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (*DataSize < sizeof (PS5_THERMAL)) {
    *DataSize = sizeof (PS5_THERMAL);
    return EFI_BUFFER_TOO_SMALL;
  }

  if (Data == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!Ps5TryLock ()) {
    return EFI_DEVICE_ERROR;
  }

  mPs5Calls++;
  Ps5ReadThermal (&Thermal);
  Ps5Unlock ();

  CopyMem (Data, &Thermal, sizeof (Thermal));
  *DataSize = sizeof (PS5_THERMAL);
  if (Attributes != NULL) {
    *Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
Ps5SetVariable (
  IN CHAR16    *VariableName,
  IN EFI_GUID  *VendorGuid,
  IN UINT32    Attributes,
  IN UINTN     DataSize,
  IN VOID      *Data
  )
{
  UINT8       Mode;
  EFI_STATUS  Status;

  if (Ps5IsOurVariable (VariableName, VendorGuid, mPs5WarmName)) {
    if ((DataSize != 1) || (Data == NULL)) {
      return EFI_INVALID_PARAMETER;
    }

    return Ps5WarmSetArmed (*(UINT8 *)Data);
  }

  if (!Ps5IsOurVariable (VariableName, VendorGuid, mPs5FanModeName)) {
    return mPs5OrigSetVariable (VariableName, VendorGuid, Attributes, DataSize, Data);
  }

  if ((DataSize != 1) || (Data == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Mode = *(UINT8 *)Data;
  if ((Mode != PS5_FAN_AUTO) && (Mode != PS5_FAN_MAX)) {
    return EFI_INVALID_PARAMETER;
  }

  mPs5SetCalls++;                           // counted once validated, before reachability
  if (!Ps5TryLock ()) {
    return EFI_DEVICE_ERROR;
  }

  Status = Ps5SetFanMode (Mode);
  Ps5Unlock ();
  return Status;
}

/**
  Called from the ExitBootServices notification: replace the two variable services and fix the table CRC.
**/
VOID
Ps5VariablesInstall (
  VOID
  )
{
  if (mPs5VarsInstalled || (gRT == NULL) || (gRT->GetVariable == NULL) || (gRT->SetVariable == NULL)) {
    return;
  }

  mPs5OrigGetVariable = gRT->GetVariable;
  mPs5OrigSetVariable = gRT->SetVariable;
  gRT->GetVariable    = Ps5GetVariable;
  gRT->SetVariable    = Ps5SetVariable;
  gRT->Hdr.CRC32      = 0;
  gRT->Hdr.CRC32      = CalculateCrc32 (gRT, gRT->Hdr.HeaderSize);
  mPs5VarsInstalled   = TRUE;
}

/**
  Called from the SetVirtualAddressMap notification. The table entries themselves are converted by the
  runtime core; only our saved copies of the original services need converting here.
**/
VOID
Ps5VariablesConvert (
  VOID
  )
{
  if (!mPs5VarsInstalled) {
    return;
  }

  EfiConvertPointer (0, (VOID **)&mPs5OrigGetVariable);
  EfiConvertPointer (0, (VOID **)&mPs5OrigSetVariable);
}
