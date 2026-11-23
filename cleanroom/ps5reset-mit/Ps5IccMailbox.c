// SPDX-License-Identifier: MIT
//
// PS5 ICC mailbox transport, root port 00:01.1 save/restore, delays.
// Written from windows-handoff/cleanroom/spec-icc-reset.md only. See LICENSE in this folder.
//
// Copyright (c) 2026 ps5win contributors

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>

#include "Ps5Icc.h"

//
// Mapped addresses of the two MMIO pages. Physical at boot, converted at SetVirtualAddressMap.
// NULL means "not usable" (conversion failed).
//
volatile UINT8    *mPs5Doorbell = (volatile UINT8 *)(UINTN)PS5_DOORBELL_PHYS;
volatile UINT8    *mPs5Mailbox  = (volatile UINT8 *)(UINTN)PS5_MAILBOX_PHYS;

PS5_BRIDGE_STATE  mPs5BootBridge;

STATIC UINT16     mPs5MsgId = ICC_ID_START;

//
// Register offsets saved for the root port, in this order.
//
STATIC CONST UINT8  mRpRegs[RP_NUM_REGS] = { 0x04, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C };

#define RPI_CMD   0
#define RPI_BUS   1
#define RPI_IO    2
#define RPI_MEM   3

// ---------------------------------------------------------------------------------------------------------------
// Delay
// ---------------------------------------------------------------------------------------------------------------

VOID
Ps5DelayMs (
  IN UINTN  Ms
  )
{
  UINT64  Start;
  UINT64  Ticks;

  Ticks = (UINT64)Ms * PS5_TSC_PER_MS;
  Start = AsmReadTsc ();
  while ((AsmReadTsc () - Start) < Ticks) {
    CpuPause ();
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Root port 00:01.1 through configuration mechanism 1 (0xCF8 / 0xCFC). 0xCF8 is restored after each access.
// ---------------------------------------------------------------------------------------------------------------

#define PCI_CF8(Reg) \
  (0x80000000U | ((UINT32)RP_BUS << 16) | ((UINT32)RP_DEV << 11) | ((UINT32)RP_FUN << 8) | ((UINT32)(Reg) & 0xFC))

UINT32
Ps5RootPortRead (
  IN UINT8  Reg
  )
{
  UINT32  Old;
  UINT32  Value;

  Old = IoRead32 (0xCF8);
  IoWrite32 (0xCF8, PCI_CF8 (Reg));
  Value = IoRead32 (0xCFC);
  IoWrite32 (0xCF8, Old);
  return Value;
}

STATIC
VOID
Ps5RootPortWrite32 (
  IN UINT8   Reg,
  IN UINT32  Value
  )
{
  UINT32  Old;

  Old = IoRead32 (0xCF8);
  IoWrite32 (0xCF8, PCI_CF8 (Reg));
  IoWrite32 (0xCFC, Value);
  IoWrite32 (0xCF8, Old);
}

STATIC
VOID
Ps5RootPortWrite16 (
  IN UINT8   Reg,
  IN UINT16  Value
  )
{
  UINT32  Old;

  Old = IoRead32 (0xCF8);
  IoWrite32 (0xCF8, PCI_CF8 (Reg));
  IoWrite16 (0xCFC + (Reg & 2), Value);
  IoWrite32 (0xCF8, Old);
}

//
// Write one saved register. The command register (0x04) and the I/O base/limit + secondary status
// register (0x1C) are written as their low 16 bits only, so the RW1C status halves are never touched.
//
STATIC
VOID
Ps5RootPortWriteSaved (
  IN UINTN   Index,
  IN UINT32  Value
  )
{
  if ((Index == RPI_CMD) || (Index == RPI_IO)) {
    Ps5RootPortWrite16 (mRpRegs[Index], (UINT16)Value);
  } else {
    Ps5RootPortWrite32 (mRpRegs[Index], Value);
  }
}

STATIC
BOOLEAN
Ps5RootPortPresent (
  VOID
  )
{
  return (BOOLEAN)(Ps5RootPortRead (0x00) == RP_ID);
}

VOID
Ps5BridgeSnapshot (
  OUT UINT32  *Regs
  )
{
  UINTN  Index;

  for (Index = 0; Index < RP_NUM_REGS; Index++) {
    Regs[Index] = Ps5RootPortRead (mRpRegs[Index]);
  }
}

BOOLEAN
Ps5BridgeUsable (
  IN CONST UINT32  *Regs
  )
{
  UINT32  Base;
  UINT32  Limit;

  if (((Regs[RPI_BUS] >> 8) & 0xFF) != RP_SEC_BUS) {
    return FALSE;
  }

  if ((Regs[RPI_CMD] & BIT1) == 0) {
    return FALSE;
  }

  //
  // Memory base/limit (type 1 header, offset 0x20): bits 15:4 = base[31:20], bits 31:20 = limit[31:20].
  //
  Base  = (Regs[RPI_MEM] & 0x0000FFF0U) << 16;
  Limit = (Regs[RPI_MEM] & 0xFFF00000U) | 0x000FFFFFU;
  return (BOOLEAN)((Base <= RP_WIN_LOW) && (Limit >= RP_WIN_HIGH));
}

VOID
Ps5BridgeSaveAtBoot (
  VOID
  )
{
  ZeroMem (&mPs5BootBridge, sizeof (mPs5BootBridge));
  if (!Ps5RootPortPresent ()) {
    return;
  }

  Ps5BridgeSnapshot (mPs5BootBridge.Reg);
  mPs5BootBridge.Valid = Ps5BridgeUsable (mPs5BootBridge.Reg);
}

STATIC
BOOLEAN
Ps5BridgeDiffers (
  IN CONST UINT32  *Cur
  )
{
  UINTN  Index;

  if ((Cur[RPI_CMD] & 0xFFFF) != (mPs5BootBridge.Reg[RPI_CMD] & 0xFFFF)) {
    return TRUE;
  }

  //
  // 0x1C: compare I/O base/limit only; the upper half is the (dynamic) secondary status.
  //
  if ((Cur[RPI_IO] & 0xFFFF) != (mPs5BootBridge.Reg[RPI_IO] & 0xFFFF)) {
    return TRUE;
  }

  for (Index = 1; Index < RP_NUM_REGS; Index++) {
    if ((Index != RPI_IO) && (Cur[Index] != mPs5BootBridge.Reg[Index])) {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Make sure the root port forwards the glue's BAR2/BAR4 window. If the current state differs from what
  was saved at boot, write back 0x18..0x2C and then the command register (16-bit), and re-check.

  @param[out] Found     Optional: the 7 registers as found before any change.
  @param[out] Restored  Optional: TRUE if registers were written.

  @retval TRUE  The bridge is usable now.
**/
BOOLEAN
Ps5BridgeEnsure (
  OUT UINT32   *Found      OPTIONAL,
  OUT BOOLEAN  *Restored   OPTIONAL
  )
{
  UINT32  Cur[RP_NUM_REGS];
  UINTN   Index;

  if (Restored != NULL) {
    *Restored = FALSE;
  }

  if (!Ps5RootPortPresent ()) {
    if (Found != NULL) {
      ZeroMem (Found, sizeof (Cur));
    }

    return FALSE;
  }

  Ps5BridgeSnapshot (Cur);
  if (Found != NULL) {
    CopyMem (Found, Cur, sizeof (Cur));
  }

  if (!mPs5BootBridge.Valid || !Ps5BridgeDiffers (Cur)) {
    return Ps5BridgeUsable (Cur);
  }

  for (Index = 1; Index < RP_NUM_REGS; Index++) {
    Ps5RootPortWriteSaved (Index, mPs5BootBridge.Reg[Index]);
  }

  Ps5RootPortWriteSaved (RPI_CMD, mPs5BootBridge.Reg[RPI_CMD]);
  if (Restored != NULL) {
    *Restored = TRUE;
  }

  Ps5BridgeSnapshot (Cur);
  return Ps5BridgeUsable (Cur);
}

/**
  Put back exactly the state found before Ps5BridgeEnsure(): command register first, then the others.
**/
VOID
Ps5BridgePutBack (
  IN CONST UINT32  *Found
  )
{
  UINTN  Index;

  Ps5RootPortWriteSaved (RPI_CMD, Found[RPI_CMD]);
  for (Index = 1; Index < RP_NUM_REGS; Index++) {
    Ps5RootPortWriteSaved (Index, Found[Index]);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Mailbox
// ---------------------------------------------------------------------------------------------------------------

#define DB_RD32(Off)       MmioRead32 ((UINTN)(mPs5Doorbell + (Off)))
#define DB_WR32(Off, V)    MmioWrite32 ((UINTN)(mPs5Doorbell + (Off)), (V))
#define MB_RD16(Off)       MmioRead16 ((UINTN)(mPs5Mailbox + (Off)))
#define MB_WR16(Off, V)    MmioWrite16 ((UINTN)(mPs5Mailbox + (Off)), (V))
//
// The message areas (+0x000 and +0x800) are accessed byte-wide only; 16-bit accesses are used only for the
// four flag registers (the known-working access widths).
//
#define MB_RD8(Off)        MmioRead8 ((UINTN)(mPs5Mailbox + (Off)))
#define MB_WR8(Off, V)     MmioWrite8 ((UINTN)(mPs5Mailbox + (Off)), (V))
#define MB_RD16_BYTES(Off) ((UINT16)(MB_RD8 (Off) | ((UINT16)MB_RD8 ((Off) + 1) << 8)))

BOOLEAN
Ps5MailboxSane (
  VOID
  )
{
  if ((mPs5Doorbell == NULL) || (mPs5Mailbox == NULL)) {
    return FALSE;
  }

  if (((DB_RD32 (DB_INT_STATUS) | DB_RD32 (DB_INT_MASK)) & ~(UINT32)0x3) != 0) {
    return FALSE;
  }

  return (BOOLEAN)((MB_RD16 (MB_HOST_WROTE) <= 1) && (MB_RD16 (MB_HOST_READ) <= 1) &&
                   (MB_RD16 (MB_CTRL_WROTE) <= 1) && (MB_RD16 (MB_CTRL_READ) <= 1));
}

VOID
Ps5MailboxMaskOff (
  VOID
  )
{
  DB_WR32 (DB_INT_MASK, 0);
}

STATIC
VOID
Ps5MailboxAck (
  VOID
  )
{
  MB_WR16 (MB_CTRL_WROTE, 0);
  MB_WR16 (MB_CTRL_READ, 1);
  MemoryFence ();
  DB_WR32 (DB_DOORBELL, DB_RING_ACK);
}

/**
  Send one request and wait for its reply.

  @param[in]  Service    Header byte 1.
  @param[in]  Type       Header bytes 2-3.
  @param[in]  Data       Request data (bytes 12..), may be NULL when DataLen is 0.
  @param[in]  DataLen    At most ICC_MSG_LEN - ICC_HDR_SIZE.
  @param[in]  TimeoutMs  Poll budget, 1 ms steps.
  @param[out] Reply      Optional buffer for the reply bytes.
  @param[in]  ReplySize  Size of Reply.
  @param[out] ReplyLen   Optional: length field of the reply (may be out of range; caller checks).

  @retval TRUE   A message with the reply bit arrived.
  @retval FALSE  Timeout, or the mailbox is not mapped.
**/
BOOLEAN
Ps5IccTransact (
  IN  UINT8        Service,
  IN  UINT16       Type,
  IN  CONST UINT8  *Data,
  IN  UINTN        DataLen,
  IN  UINTN        TimeoutMs,
  OUT UINT8        *Reply      OPTIONAL,
  IN  UINTN        ReplySize,
  OUT UINT16       *ReplyLen   OPTIONAL
  )
{
  UINT8   Bytes[ICC_MSG_LEN];
  UINT16  Sum;
  UINTN   Index;
  UINTN   Elapsed;
  UINT32  Status;
  UINT16  RxType;
  UINT16  RxLen;
  UINTN   Copy;

  if ((mPs5Doorbell == NULL) || (mPs5Mailbox == NULL) || (DataLen > ICC_MSG_LEN - ICC_HDR_SIZE)) {
    return FALSE;
  }

  //
  // Build the request.
  //
  ZeroMem (Bytes, sizeof (Bytes));
  Bytes[ICC_OFF_MAGIC]     = ICC_MAGIC;
  Bytes[ICC_OFF_SERVICE]   = Service;
  WriteUnaligned16 ((UINT16 *)(Bytes + ICC_OFF_TYPE), Type);
  WriteUnaligned16 ((UINT16 *)(Bytes + ICC_OFF_CONST), ICC_HDR_CONST);
  WriteUnaligned16 ((UINT16 *)(Bytes + ICC_OFF_ID), mPs5MsgId++);
  WriteUnaligned16 ((UINT16 *)(Bytes + ICC_OFF_LEN), ICC_MSG_LEN);
  if (DataLen != 0) {
    CopyMem (Bytes + ICC_HDR_SIZE, Data, DataLen);
  }

  Sum = 0;
  for (Index = 0; Index < ICC_MSG_LEN; Index++) {
    Sum = (UINT16)(Sum + Bytes[Index]);
  }

  WriteUnaligned16 ((UINT16 *)(Bytes + ICC_OFF_CSUM), Sum);

  //
  // A controller message still waiting: acknowledge it first.
  //
  if (MB_RD16 (MB_CTRL_WROTE) != 0) {
    Ps5MailboxAck ();
  }

  DB_WR32 (DB_INT_STATUS, 0x3);
  MB_WR16 (MB_HOST_READ, 0);
  for (Index = 0; Index < ICC_MSG_LEN; Index++) {
    MB_WR8 (MB_TX + Index, Bytes[Index]);
  }

  MB_WR16 (MB_HOST_WROTE, 1);
  MemoryFence ();
  DB_WR32 (DB_DOORBELL, DB_RING_SENT);

  //
  // Poll.
  //
  for (Elapsed = 0; Elapsed < TimeoutMs; Elapsed++) {
    Ps5DelayMs (1);

    Status = DB_RD32 (DB_INT_STATUS);
    if (Status != 0) {
      DB_WR32 (DB_INT_STATUS, Status);
    }

    if (((Status & BIT0) == 0) && (MB_RD16 (MB_CTRL_WROTE) == 0)) {
      continue;
    }

    RxType = MB_RD16_BYTES (MB_RX + ICC_OFF_TYPE);
    RxLen  = MB_RD16_BYTES (MB_RX + ICC_OFF_LEN);

    if (((RxType & ICC_REPLY_BIT) != 0) && (Reply != NULL) &&
        (RxLen >= ICC_REPLY_MIN) && (RxLen <= ICC_REPLY_MAX))
    {
      Copy = MIN ((UINTN)RxLen, ReplySize);
      for (Index = 0; Index < Copy; Index++) {
        Reply[Index] = MB_RD8 (MB_RX + Index);
      }
    }

    Ps5MailboxAck ();

    if ((RxType & ICC_REPLY_BIT) != 0) {
      if (ReplyLen != NULL) {
        *ReplyLen = RxLen;
      }

      return TRUE;
    }
  }

  return FALSE;
}

/**
  Thermal / fan query: send, then check reply length (>= 0x10) and the 16-bit status at data +0.

  @return 0 and *Value = 16-bit value at data +2, or PS5_ERR_NO_REPLY / PS5_ERR_BAD_LEN / PS5_ERR_STATUS.
**/
#define PS5_QUERY_TIMEOUT_MS  1000

INT32
Ps5IccQueryValue (
  IN  UINT8        Service,
  IN  UINT16       Type,
  IN  CONST UINT8  *Data,
  IN  UINTN        DataLen,
  OUT UINT16       *Value
  )
{
  UINT8   Reply[0x40];
  UINT16  Len;

  *Value = 0;
  ZeroMem (Reply, sizeof (Reply));
  Len = 0;
  if (!Ps5IccTransact (Service, Type, Data, DataLen, PS5_QUERY_TIMEOUT_MS, Reply, sizeof (Reply), &Len)) {
    return PS5_ERR_NO_REPLY;
  }

  if ((Len < 0x10) || (Len > ICC_REPLY_MAX)) {
    return PS5_ERR_BAD_LEN;
  }

  if (ReadUnaligned16 ((UINT16 *)(Reply + ICC_HDR_SIZE)) != 0) {
    return PS5_ERR_STATUS;
  }

  *Value = ReadUnaligned16 ((UINT16 *)(Reply + ICC_HDR_SIZE + 2));
  return 0;
}
