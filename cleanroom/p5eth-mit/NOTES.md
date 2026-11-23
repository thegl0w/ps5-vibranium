# p5eth-mit: clean-room MIT Ethernet miniport

Written 2026-11-21. The only sources used were `windows-handoff/cleanroom/spec-p5eth.md`, public NDIS/WDK
documentation, and the WDK headers in `windows-handoff/build/wdk/` (to check struct revisions). I did not open any
file on the brief's forbidden list.

## Files
- `p5eth.cpp`: the driver, `SPDX-License-Identifier: MIT` with the full MIT text.
- `p5eth.inf`: hardware ID `root\p5eth`, service `p5eth`, class Net, `CatalogFile = p5eth.cat`, `NTamd64.10.0`,
  DIRID 13 (run from the driver store), `Characteristics = 0x4` (NCF_PHYSICAL), BusType 0, plus the standard
  `*IfType`/`*MediaType`/`*PhysicalMediaType` keywords.
- `build.sh`: clang-cl + lld-link with exactly the flags from the brief. Output:
  `windows-handoff/build/cleanroom-p5eth/p5eth-unsigned.sys`, **unsigned**. The source is compiled from a copy
  in the output folder under a relative name, `/debug:none` is set, and the script fails if `strings` finds
  `/Users` in the binary.

## Structure
- `DriverEntry` registers NDIS 6.30 characteristics (revision 2).
- `P5Initialize`, in order: registration attributes (internal interface type, hardware device); root port 00:01.1
  check/fix; check of 40:00.2 (ID + BAR0) and command bits; map BAR0 (4 KiB, uncached) and the DMA range
  (0x6F300000, 0x225000 bytes, cached); read the MAC from 0x85403000 (page mapped and then unmapped again;
  random LAA if the address is invalid); allocate the RX NBL/MDL/copy buffer; start the chip as in the spec's
  Start sequence; general attributes with the current link state; start the worker thread.
- Worker thread (`WorkerThread`): link poll every 200 ms → `NDIS_STATUS_LINK_STATE`; ISR read and acknowledge;
  RX up to 256 frames per pass; RX kick after frames or when ISR bit 1 or 5 is set; TX reclaim; TX from the
  queue. Idle: 32 × 2 µs spins, then a 1 ms wait on the wake event. Timer resolution is 1 ms while it runs.
  Under sustained load it yields every 64 busy passes. It runs at normal priority, never realtime.
- RX: one driver-owned 0x600 buffer and one NBL, indicated with `NDIS_RECEIVE_FLAGS_RESOURCES`, so
  `ReturnNetBufferLists` is never called for them.
- TX: `P5Send` queues under a spin lock and sets the event. The thread copies each NET_BUFFER into a 0x800-byte
  slot (padded to 60), fills descriptors as the spec says, fences, kicks, and completes the NBL right after the
  copy. An NBL is taken only when the ring has room for all of its NET_BUFFERs.
- Pause/Restart: Pause changes state under the queue lock, flushes the queue (`NDIS_STATUS_PAUSED`), then
  waits until the thread's `Busy` counter (in-flight RX indication or TX NBL) reaches 0.
- Halt: stop the thread → flush the queue → chip Stop → free and unmap → restore the root port if it was
  changed. Shutdown: chip Stop with busy-wait polling only, since it can run at raised IRQL.
- OIDs: the usual 802.3/general query set, OID_GEN_STATISTICS (frame/byte counters split into
  unicast/multicast/broadcast), and sets for the packet filter (all 5 allowed, no chip change), lookahead and
  the multicast list (≤ 32, stored, ignored by the hardware).
- Registry diagnostics under `Services\p5eth` (PASSIVE only): `Stage`, `State`, `Link`, `RxOk`, `TxOk`, `RxErr`,
  `TxErr`, `RpBusNum`/`RpMemWin`/`RpCommand`/`RpChanged`, `PciId`/`PciBar0`, `MacHi`/`MacLo`/`MacRandom`,
  `McastFail`. Stage numbers 1-6 are init progress, 90-96 are init failures, 10-11 are halt.

## NVMe safety
- Every config access goes through `CfgAllowed()`, which allows only 00:01.1 and 40:00.2. I checked the
  disassembly: the only 0xCF8 addresses in the binary are 0x80000904/18/20 (root port) and 0x80400200/04/10
  (40:00.2). 40:00.0 (0x804000xx) does not appear.
- The only MMIO mappings are BAR0 0x85000000 (4 KiB) and the MAC page 0x85403000. DMA addresses stay within
  0x6F300000..0x6F524FFF (compile-time `C_ASSERT`s on the layout).
- Interrupts are disabled around each 0xCF8/0xCFC pair with `pushfq; cli … sti` (only if IF was set before), and
  the previous value of 0xCF8 is restored.

## Checks
- `./build.sh`: no errors, no warnings. 21.5 KB native PE, subsystem 10.0. Imports: NDIS.SYS, ntoskrnl.exe, HAL.dll.
- `strings p5eth-unsigned.sys | grep /Users` gives no output.
- I reviewed the code against each spec section: platform facts (root port values, the rule for keeping the top
  byte, 16-bit command writes, restore at halt, function ID/BAR check, BAR0, MAC rules, DMA range/layout);
  every register row; the Stop and Start sequences step by step in order; the RX ring (1024 × 0x600, wrap bit,
  w1-w3 before w0, the 60..0x600 check, clamp to 1514, give-back, kick rule); the TX ring (256, initial
  values, w2 = 4, flags 0x30000000 and wrap, fence + kick, the reclaim condition, the ring-full rule); No-PHY
  (none is done); and the Windows side (deserialized 6.30, internal, 802.3, MTU 1500, 1 Gb/s full, filters,
  multicast 32, one thread, 200 ms link poll, 256 RX per pass, spin then 1 ms wait, 1 ms timer resolution,
  RESOURCES receive, registry diagnostics, halt order).
- Not tested on hardware. The main session signs and tests it.

## Spec gaps
Listed in `../p5eth-questions.md`: whether RX length includes the FCS, what to do when multicast hash waits time
out, whether 0x9C needs a delay, TX slot stride, root port restore order, 40:00.2 command at halt, other ISR
bits, and what the 0x110/0x114 registers count.

**Answered** in `../p5eth-answers.md` (spec author, 2026-11-21). All 8 answers match the code as built, so no code
changes and no rebuild: (1) use the length as given, clamp to 1514 (whether it includes the FCS is unknown);
(2) carry on when multicast waits time out, counting failures is fine; (3) no delay at 0x9C; (4) 128-byte
alignment is the only rule for TX slots, so the fixed 0x800 slot is fine; (5) restore command, then 0x20, then
0x18, which is what the code does; (6) leave 40:00.2's command bits set; (7) act only on ISR bits 1 and 5 and
acknowledge all bits; (8) read 0x110/0x114 once at start and discard.

## Choices that weren't in the spec (my own)
- Sends fail with `NDIS_STATUS_MEDIA_DISCONNECTED` while the link is down, and with `NDIS_STATUS_PAUSED` when
  not running.
- `NCF_PHYSICAL` and `NDIS_MINIPORT_ATTRIBUTES_HARDWARE_DEVICE` (it is real hardware, even though it's
  root-enumerated). If NetCfg objects to this on the console, use `Characteristics = 0x1` (NCF_VIRTUAL) and drop
  the attribute flag.
- `NDIS_MAC_OPTION_NO_LOOPBACK`: NDIS does loopback itself.
