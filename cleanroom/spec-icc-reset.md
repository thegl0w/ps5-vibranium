# Clean-room spec: PS5 power off / restart and thermal readout from the UEFI payload (target: MIT ResetSystemLib)

Facts only: addresses, register meanings, message formats, order of operations. No code. Written 2026-11-21 by the
session that has seen the GPL implementation; the implementer must not open the GPL sources (see the brief).

## What the component is

An EDK2 `ResetSystemLib` instance (library class `ResetSystemLib|DXE_RUNTIME_DRIVER`, module type
DXE_RUNTIME_DRIVER, with a constructor), linked only into `MdeModulePkg/Universal/ResetSystemRuntimeDxe` of the
project's UefiPayloadPkg build. The console has no ACPI reset or power-off; Windows ends in the UEFI ResetSystem
runtime service, so this library must make the console power off or restart by asking Sony's southbridge controller
(the "EMC") over the ICC mailbox. It also answers two UEFI variable names for temperatures and fan mode (below).
Library API: ResetCold, ResetWarm, ResetShutdown, ResetPlatformSpecific, ResetSystem (shutdown for EfiResetShutdown,
restart for every other type). Before ExitBootServices: do nothing but halt (CpuDeadLoop). After it: send the power
messages; if anything fails, halt.

## Hardware

- Salina "glue" function 40:00.5 (104d:9107) behind root port 00:01.1 (1022:13DF). Addresses as the previous OS
  assigned them on this console: BAR2 = 0x85200000, BAR4 = 0x85400000. The firmware never enumerates bus 0x40.
- **Doorbell block**: physical 0x85308000 (BAR2 + 0x108000). 32-bit registers:
  +0x04 doorbell: write 1 = "host sent a message", write 2 = "host acknowledged the controller's message".
  +0x14 interrupt status: bit 0 = controller sent/replied, bit 1 = ack; write the bits to clear them.
  +0x24 interrupt mask: write 0 before polling (as done when the OS stops using the interrupt).
- **Mailbox page**: physical 0x85400000 (BAR4), at least 4 KiB. Host-to-controller message area at +0x000,
  controller-to-host area at +0x800. 16-bit flag registers: +0x7F0 "host wrote" (1 = message ready), +0x7F4 "host
  read" (host sets 0 before writing), +0xFF0 "controller wrote" (nonzero = a message from the controller waits),
  +0xFF4 "controller message read" (host sets 1 after reading).
- Sanity check before use: interrupt status and mask have no bits above bit 1, and all four flags read 0 or 1.
- Both pages must be marked EFI_MEMORY_RUNTIME (the project's boot-manager code already does this) and their pointers
  converted at SetVirtualAddressMap.
- Root port 00:01.1: at constructor time (boot), save its registers 0x04, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C via
  configuration mechanism 1 (ports 0xCF8/0xCFC, bus 0 dev 1 fn 1; check ID 0x13DF1022 first; restore 0xCF8 after).
  The saved state is usable when secondary bus = 0x40, the memory window (reg 0x20) covers 0x85200000 through
  0x85400000 + 0x7FFFF, and command bit 1 (memory) is set. At reset time, if the current values differ (command
  compared in its low 16 bits), write the saved registers 0x18-0x2C back, then the command register last as a 16-bit
  write, and re-check. For the thermal/fan calls (which return to Windows), restore the same way before talking to
  the controller, and afterwards put back exactly what Windows had (command register first, then the others).

## Message format (all little-endian)

| Bytes | Field |
|---|---|
| 0 | magic 0x42 |
| 1 | service |
| 2-3 | type |
| 4-5 | 0x0003 |
| 6-7 | message id: a counter, incremented per message (any start value; 0x5000 used) |
| 8-9 | total length in bytes including the header; minimum and used value 0x20 |
| 10-11 | checksum: 16-bit sum of all `length` bytes of the message, computed with this field zero |
| 12.. | data |

A reply has bit 0x4000 set in its type; its length (bytes 8-9) must be 12..0x7F0.

**Send and wait**: if "controller wrote" is nonzero, acknowledge first (controller wrote := 0, controller message read
:= 1, doorbell := 2). Clear interrupt status (write 3). Host read := 0. Copy the message bytes to +0x000. Host wrote := 1.
Memory fence. Doorbell := 1. Then poll every 1 ms until the timeout: read interrupt status and write any set bits back;
if status bit 0 is set or "controller wrote" is nonzero, read the reply's type and length from +0x802 and +0x808, copy
the reply if needed, then acknowledge (controller wrote := 0, controller message read := 1, doorbell := 2). Done when
the reply has the reply bit.

## Messages

**Power off / restart**: mask := 0. Then service 2 (general) type 0x10, sent twice: data bytes [0, 0], then [1, 0]
(2 s timeout each; if one gets no reply, skip to the power message). Then service 4 (power) type 1 with data bytes
[0, action, 2, 0, 1], action 0 = power off, 1 = restart (5 s timeout). Then wait (about 10 s); the controller cuts or
resets power. Nothing is ever sent to the PSP/SMU.

**Thermal and fan** (4-byte request data at byte 12; the reply's data has a 16-bit status at +0 that must be 0, and a
value at +2; reply length must be at least 0x10, and at least 16 to carry the value):
- Temperature: service 0x0B, type 1, request [sensor, 0, 0, 0]. Value: signed 16-bit, °C × 256; valid 0 ≤ v < 125×256;
  report millidegrees = (v × 1000 + 128) / 256. Sensors used: 0, 1, 2, 3 (MainSoC, Local1-3) and 18.
- Fan duty: service 0x0A, type 1, no request data. Value 0..1024.
- Fan mode read: service 0x0A, type 3, request [0, 0, 0, 0]. Value's low byte: 1 auto, 2 maximum, 4 manual.
- Fan mode set: service 0x0A, type 2, request [0, mode, 0, 0]. Policy: only 1 (auto, controller drives the fan) and
  2 (maximum) may be set.

## UEFI variable interface (the project's own design, unchanged)

Installed at ExitBootServices by replacing gRT->GetVariable/SetVariable (recompute the table CRC); originals called
for everything else; pointers converted at SetVirtualAddressMap.
- GUID {0x8c0f1e5a, 0x5053, 0x3543, {0x8a, 0x6b, 0x5f, 0x4e, 0x2d, 0x1c, 0x0b, 0x9a}}.
- GetVariable "Ps5Thermal": 64-byte packed struct, read fresh on every call: UINT32 Version (1); UINT32 Flags (bit 0
  bridge state known from boot, bit 1 bridge had to be restored for this read, bit 2 bridge usable, bit 3 mailbox sane);
  INT32 TempMilliC[5] (sensors 0,1,2,3,18; negative = error: -1 no reply, -2 bad length, -3 status not 0,
  -4 out of range); INT32 Duty (negative = error); INT32 Mode (negative = error); UINT32 Bridge[4] (root port registers
  0x04, 0x18, 0x20, 0x24 as found); UINT32 Calls; UINT32 SetCalls; UINT32 Reserved. Attributes BS|RT.
  Too-small buffer: EFI_BUFFER_TOO_SMALL with the size.
- SetVariable "Ps5FanMode": exactly 1 byte, 1 or 2; anything else EFI_INVALID_PARAMETER; controller unreachable or
  error: EFI_DEVICE_ERROR.

## Optional diagnostics (project's own)

During reset at runtime, coloured squares are drawn through the GPU's MMIO index/data I/O ports (0x2000 index,
0x2004 data, 0x2018 index-high = 0; framebuffer at VRAM offset 0x0F000000, pitch 10240 bytes, 32-bit pixels): one
square per step (reached, bridge state, mailbox sane, each message answered or not, done). Keep or drop.
Delays: TSC-based, 1,596,300 ticks per ms on this console.
