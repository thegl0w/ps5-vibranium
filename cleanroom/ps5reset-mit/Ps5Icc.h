// SPDX-License-Identifier: MIT
//
// PS5 southbridge (EMC) ICC mailbox: shared definitions for the ResetSystemLib instance.
// Written from windows-handoff/cleanroom/spec-icc-reset.md only.
//
// Copyright (c) 2026 ps5win contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
// to permit persons to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of
// the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
// CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

#ifndef PS5_ICC_H_
#define PS5_ICC_H_

#include <Uefi.h>

//
// Physical addresses (as assigned by the previous OS; the firmware never enumerates bus 0x40).
//
#define PS5_DOORBELL_PHYS  0x85308000ULL       // glue 40:00.5 BAR2 + 0x108000
#define PS5_MAILBOX_PHYS   0x85400000ULL       // glue 40:00.5 BAR4, >= 4 KiB

//
// Doorbell block registers (32-bit).
//
#define DB_DOORBELL        0x04
#define   DB_RING_SENT       1                 // host sent a message
#define   DB_RING_ACK        2                 // host acknowledged the controller's message
#define DB_INT_STATUS      0x14                // bit0 = controller sent/replied, bit1 = ack; write-1-to-clear
#define DB_INT_MASK        0x24

//
// Mailbox page layout.
//
#define MB_TX              0x000               // host -> controller message area
#define MB_RX              0x800               // controller -> host message area
#define MB_HOST_WROTE      0x7F0               // 16-bit
#define MB_HOST_READ       0x7F4               // 16-bit
#define MB_CTRL_WROTE      0xFF0               // 16-bit
#define MB_CTRL_READ       0xFF4               // 16-bit

//
// Message header (little-endian).
//
#define ICC_MAGIC          0x42
#define ICC_HDR_CONST      0x0003
#define ICC_HDR_SIZE       12
#define ICC_MSG_LEN        0x20                // fixed request length
#define ICC_REPLY_BIT      0x4000
#define ICC_REPLY_MIN      12
#define ICC_REPLY_MAX      0x7F0
#define ICC_ID_START       0x5000

#define ICC_OFF_MAGIC      0
#define ICC_OFF_SERVICE    1
#define ICC_OFF_TYPE       2
#define ICC_OFF_CONST      4
#define ICC_OFF_ID         6
#define ICC_OFF_LEN        8
#define ICC_OFF_CSUM       10

//
// Services / types.
//
#define ICC_SVC_GENERAL    0x02
#define ICC_SVC_POWER      0x04
#define ICC_SVC_FAN        0x0A
#define ICC_SVC_THERMAL    0x0B

#define ICC_GENERAL_T10    0x10
#define ICC_POWER_T1       0x01
#define ICC_FAN_DUTY       0x01
#define ICC_FAN_MODE_SET   0x02
#define ICC_FAN_MODE_GET   0x03
#define ICC_THERMAL_READ   0x01

#define PS5_POWER_OFF      0
#define PS5_POWER_RESTART  1

#define PS5_FAN_AUTO       1
#define PS5_FAN_MAX        2

//
// Root port 00:01.1 (1022:13DF), configuration mechanism 1.
//
#define RP_BUS             0
#define RP_DEV             1
#define RP_FUN             1
#define RP_ID              0x13DF1022U
#define RP_SEC_BUS         0x40
#define RP_WIN_LOW         0x85200000U
#define RP_WIN_HIGH        (0x85400000U + 0x7FFFFU)

#define RP_NUM_REGS        7                   // 0x04, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C

typedef struct {
  BOOLEAN    Valid;                            // saved at boot and usable
  UINT32     Reg[RP_NUM_REGS];
} PS5_BRIDGE_STATE;

//
// Timing: TSC ticks per millisecond on this console.
//
#define PS5_TSC_PER_MS     1596300ULL

//
// Results of a thermal/fan query (negative = error), also used in Ps5Thermal.
//
#define PS5_ERR_NO_REPLY   (-1)
#define PS5_ERR_BAD_LEN    (-2)
#define PS5_ERR_STATUS     (-3)
#define PS5_ERR_RANGE      (-4)

//
// UEFI variable interface.
//
#define PS5_VAR_GUID \
  { 0x8c0f1e5a, 0x5053, 0x3543, { 0x8a, 0x6b, 0x5f, 0x4e, 0x2d, 0x1c, 0x0b, 0x9a } }

#define PS5_THERMAL_VERSION        1
#define PS5_THERMAL_F_BOOT_STATE   BIT0        // bridge state known from boot
#define PS5_THERMAL_F_RESTORED     BIT1        // bridge had to be restored for this read
#define PS5_THERMAL_F_BRIDGE_OK    BIT2        // bridge usable
#define PS5_THERMAL_F_MAILBOX_OK   BIT3        // mailbox sane

#pragma pack(1)
typedef struct {
  UINT32    Version;
  UINT32    Flags;
  INT32     TempMilliC[5];                     // sensors 0, 1, 2, 3, 18
  INT32     Duty;
  INT32     Mode;
  UINT32    Bridge[4];                         // root port 0x04, 0x18, 0x20, 0x24 as found
  UINT32    Calls;
  UINT32    SetCalls;
  UINT32    Reserved;
} PS5_THERMAL;
#pragma pack()

STATIC_ASSERT (sizeof (PS5_THERMAL) == 64, "Ps5Thermal must be 64 bytes");

//
// Module state shared between the files (defined in Ps5IccMailbox.c).
//
extern volatile UINT8  *mPs5Doorbell;
extern volatile UINT8  *mPs5Mailbox;
extern PS5_BRIDGE_STATE mPs5BootBridge;

//
// Ps5IccMailbox.c
//
VOID
Ps5DelayMs (
  IN UINTN  Ms
  );

UINT32
Ps5RootPortRead (
  IN UINT8  Reg
  );

VOID
Ps5BridgeSnapshot (
  OUT UINT32  *Regs
  );

BOOLEAN
Ps5BridgeUsable (
  IN CONST UINT32  *Regs
  );

VOID
Ps5BridgeSaveAtBoot (
  VOID
  );

BOOLEAN
Ps5BridgeEnsure (
  OUT UINT32   *Found      OPTIONAL,
  OUT BOOLEAN  *Restored   OPTIONAL
  );

VOID
Ps5BridgePutBack (
  IN CONST UINT32  *Found
  );

BOOLEAN
Ps5MailboxSane (
  VOID
  );

VOID
Ps5MailboxMaskOff (
  VOID
  );

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
  );

INT32
Ps5IccQueryValue (
  IN  UINT8        Service,
  IN  UINT16       Type,
  IN  CONST UINT8  *Data,
  IN  UINTN        DataLen,
  OUT UINT16       *Value
  );

//
// Ps5IccVariables.c
//
VOID
Ps5VariablesInstall (
  VOID
  );

VOID
Ps5VariablesConvert (
  VOID
  );

#endif
