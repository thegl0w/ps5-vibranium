# Ps5IccResetSystemLib (MIT): notes

Clean-room `ResetSystemLib` for the PS5, written only from `../spec-icc-reset.md`, public UEFI/EDK2 docs and the
EDK2 tree's own headers (`build/edk2/MdePkg/Include`, `MdeModulePkg/Include`, plus `MdePkg.dec` for GUID names and
`UefiPayloadPkg/UefiPayloadPkg.dsc` for library mappings). No GPL sources were opened. License: MIT (`LICENSE`).

## Files

| File | Content |
|---|---|
| `Ps5IccResetSystemLib.inf` | BASE_NAME `Ps5IccResetSystemLib`, `ResetSystemLib\|DXE_RUNTIME_DRIVER`, new FILE_GUID 84C81778-4A0A-4F5E-AFA5-307057F7944F, constructor `Ps5IccResetSystemLibConstructor` |
| `Ps5Icc.h` | Addresses, register offsets, message layout, services, `PS5_THERMAL` (64 bytes, static-asserted), prototypes |
| `Ps5IccMailbox.c` | TSC delay, root port 00:01.1 save/check/restore/put-back over 0xCF8/0xCFC, mailbox sanity, send-and-wait, thermal/fan query helper |
| `Ps5IccResetSystemLib.c` | Constructor (save bridge, create ExitBootServices + VirtualAddressChange events), power sequence, the five ResetSystemLib functions |
| `Ps5IccVariables.c` | gRT->GetVariable/SetVariable hooks for `Ps5Thermal` / `Ps5FanMode`, CRC fix-up, pointer conversion |

Library classes used: BaseLib, BaseMemoryLib, IoLib, SynchronizationLib, UefiBootServicesTableLib, UefiRuntimeLib,
UefiRuntimeServicesTableLib (all mapped in UefiPayloadPkg.dsc). The runtime paths only use BaseLib/BaseMemoryLib/
IoLib/SynchronizationLib primitives and UefiRuntimeLib's EfiConvertPointer (in the VA-change event). No DebugLib.

## Design

- **Phases.** A flag set in the ExitBootServices notification. Before it, every reset call is `CpuDeadLoop()`.
  After it, `Ps5Power(action)`: bridge ensure → mailbox sanity (either failing: send nothing, halt) → mask := 0 →
  service 2 type 0x10 data [0,0] (2 s); only if that was answered, [1,0] (2 s) → service 4 type 1 data
  [0,action,2,0,1] (5 s), sent whether or not the 0x10 messages were answered → 10 s TSC wait whether or not the
  power message was answered → `CpuDeadLoop()`. ResetShutdown/EfiResetShutdown = power off (action 0); Cold, Warm,
  PlatformSpecific and every other ResetSystem type = restart (action 1).
- **Pointers.** `mPs5Doorbell` / `mPs5Mailbox` start as the physical addresses and are converted with
  `EfiConvertPointer` at SetVirtualAddressMap. If a conversion fails (page not marked EFI_MEMORY_RUNTIME) the
  pointer becomes NULL and every later use fails safely (reset halts, thermal returns errors).
- **Root port.** 7 registers (0x04, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C) saved in the constructor after an ID check;
  saved state is "valid" when secondary bus = 0x40, the memory window covers 0x85200000..0x8547FFFF and memory
  enable is set. At use: if different from the saved state, write 0x18..0x2C, then the command register (16-bit)
  last, re-read and re-check. Thermal/fan calls put back what was found afterwards: command first, then the
  others. 0xCF8 is restored after each access.
- **Send-and-wait** follows the spec order exactly (ack a pending controller message, clear status, host read := 0,
  byte copy of the 0x20-byte request, host wrote := 1, fence, doorbell 1, 1 ms polls: write back status bits,
  on status bit 0 or controller-wrote read type/length at +0x802/+0x808, copy if it is a reply with length 12..0x7F0,
  ack, finish on the reply bit). Message ids count up from 0x5000.
- **MMIO access widths** (spec author, 2026-11-21: the known-working widths; 32-bit accesses to the message areas
  were never tested on the console): the message areas (+0x000 request, +0x800 reply, including the reply's
  type/length fields at +0x802/+0x808) use 8-bit `MmioWrite8`/`MmioRead8` only; the four flag registers use 16-bit
  accesses; the doorbell block registers use 32-bit accesses.
- **Variables.** Installed in the ExitBootServices notification (originals saved, table entries replaced,
  `Hdr.CRC32` recomputed with BaseLib `CalculateCrc32`, same CRC-32 as the boot service). The runtime core converts
  the table entries; our saved originals are converted in our VA-change event. `Ps5Thermal` is read fresh each
  call; `Ps5FanMode` accepts exactly 1 byte of 1 or 2. Everything else goes to the original services.
- **Dropped:** the optional coloured-square GPU diagnostics.

## Decisions where the spec was silent (questions in `../icc-questions.md`, answers in `../icc-answers.md`)

All confirmed by the spec author on 2026-11-21 except where marked *changed*.

- *Changed:* thermal/fan query timeout 1000 ms each (worst case about 7 s for one Ps5Thermal read if nothing answers).
- Any message with the reply bit ends a wait; service/id are not matched. On the thermal/fan path a reply with
  length outside 12..0x7F0 (or below 0x10) is a failed query (-2).
- Mask := 0 also before thermal/fan exchanges, not restored.
- Register 0x1C compared and written as its low 16 bits only (upper half is RW1C status); command register likewise
  16-bit.
- Unusable bridge / insane mailbox at reset → send nothing, halt. *Changed:* an unanswered power message still gets
  the 10 s wait; unanswered 0x10 messages no longer matter to the power message (they never stopped it).
- `Calls` counts GetVariable calls that get data; `SetCalls` counts Ps5FanMode writes once validated, before the
  lock or reachability check.
- Ps5FanMode attributes ignored. *Changed:* duty > 1024 → -2. Mode = low byte, no range check.
- Concurrent variable calls → EFI_DEVICE_ERROR (try-lock); resets ignore the lock.
- Only the root port is checked/restored; the glue function 40:00.5 is never touched.
- Status failure on the fan-mode set reply → EFI_DEVICE_ERROR.

## How it was checked

Re-run after the answers were applied (same results, plus the 1 s query timeout).
Re-run again after the switch to byte-wide message-area accesses: same bytes and results, and the simulation
now also counts access widths on the mailbox page (0 violations: only 8-bit on the message areas, only 16-bit on
the flags).


1. `clang -fsyntax-only` and full `-c -O2 -mno-red-zone -Wall -Wextra -Werror` for `x86_64-unknown-linux-gnu` and
   `x86_64-pc-windows-msvc` against the EDK2 headers (`-include Base.h`, MdePkg/Include + X64, MdeModulePkg/Include):
   no warnings. AutoGen is not involved, so the INF itself is untested until the main session builds it.
2. Host simulation (scratchpad, not shipped): `Ps5IccMailbox.c` linked against stubbed IoLib/BaseLib and a fake
   controller on two host buffers. Verified: request bytes (e.g. temperature sensor 18 =
   `42 0b 01 00 03 00 00 50 20 00 d3 00 12 00 ...`, power restart = `42 04 01 00 03 00 01 50 20 00 bf 00 00 01 02 00 01 ...`),
   checksum, ack sequence, reply parsing (45.5 °C → 45500 m°C), 2 s transact timeout and 1 s query timeout
   (returns -1) when unanswered, bridge restore order and
   put-back, 0x1C status half left alone.

Not tested on the console. Integration: point `ResetSystemLib` for ResetSystemRuntimeDxe at this INF instead of the
old one; nothing else in the DSC should need to change.
