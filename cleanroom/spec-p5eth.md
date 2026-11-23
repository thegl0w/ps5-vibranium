# Clean-room spec: PS5 wired Ethernet for Windows (target: MIT `p5eth.sys`)

Facts only: hardware addresses, registers, bits, layouts and the order of operations. No code. Written 2026-11-21
by the session that has seen the GPL implementation; the implementer must not open the GPL sources (see the brief).

## Platform facts

- Controller: Sony "Salina" gigabit Ethernet, PCI function 40:00.2, ID 104d:9104. Its bus (0x40) is invisible to
  Windows: the ACPI root bridge does not cover it, and bus 0x40 also carries the console's internal NVMe
  (40:00.0, 104d:90eb), which must never be touched. So the driver is a **root-enumerated** NDIS miniport
  (hardware ID `root\p5eth`, service name `p5eth`, same INF name `p5eth.inf`, created with the project's
  `devadd.exe C:\ps5\eth\p5eth.inf root\p5eth`), reaching the chip by fixed physical addresses.
- Root port 00:01.1, ID 1022:13DF, forwards bus 0x40. Linux leaves it with: register 0x18 low 24 bits = 0x404000
  (primary 0, secondary 0x40, subordinate 0x40); register 0x20 (memory window) = 0xC4308000 (0x80000000-0xC43FFFFF);
  command register bits 0-2 set (I/O, memory, bus master). Windows may change these. Access the root port with PCI
  configuration mechanism 1 (I/O ports 0xCF8/0xCFC, bus 0 device 1 function 1) because the HAL silently drops writes
  to it; disable interrupts on the CPU around each address/data pair and restore 0xCF8's previous value. If any of the
  three differs at start, write Linux's values (bus numbers keep register 0x18's top byte; command register written as
  16 bits), and put the found values back at halt (command first is not required here; restore all three).
- Function 40:00.2's own configuration space (mechanism 1, bus 0x40 device 0 function 2) is reachable once the root
  port carries those bus numbers. Check: dword 0 = 0x9104104D; BAR0 (reg 0x10) & 0xFFFFF000 = 0x85000000. Only
  change: set command bits 1 and 2 (memory, bus master) if not both set (16-bit write of register 4).
- Register block: BAR0 at physical 0x85000000, 4 KiB, uncached.
- MAC address: six bytes at physical 0x85403000 (the Salina "glue" function's BAR4 is 0x85400000; the bytes are at
  +0x3000), read bytewise. Reject it if bit 0 of byte 0 is set (group address), if all bytes are 0, or all 0xFF; then
  use a locally administered address (first byte 0x02) with the rest derived from something random.
- DMA memory: firmware-reserved RAM 0x6F200000-0x6F5FFFFF (below 4 GiB). The first 384 KiB belong to the Wi-Fi
  driver. Ethernet uses 0x6F300000 .. 0x6F524FFF (0x225000 bytes), mapped cached; all chip-visible addresses are
  32-bit physical addresses in this range. Layout used so far (any layout inside the range works):
  TX ring 0x1000 bytes at +0, RX ring 0x4000 at +0x1000, TX buffers 0xA0000 at +0x5000, RX buffers 0x180000 at
  +0xA5000.

## Registers (32-bit, byte offsets in BAR0)

| Offset | Use |
|---|---|
| 0x04 | Link status: bit 0 = link up. |
| 0x08 | After the MAC reset: OR in 0x07597C00. |
| 0x0C | At start: clear bit 7. |
| 0x10 | Receive configuration. Bit 4 = promiscuous. At start: clear bits 0, 4, 7 (AND 0xFFFFFF6E), then OR 0x81. |
| 0x14 | MAC address high: (byte0 << 8) or byte1. |
| 0x18 | MAC address low: byte2<<24 or byte3<<16 or byte4<<8 or byte5. |
| 0x1C | Multicast hash table control. Clear: write 0x80000000, wait until bit 17 (0x20000) reads 1. Set entry e (0..255): write e or 0x7000, wait until bit 14 (0x4000) reads 0. Waits: poll with ~1 µs steps, give up after ~10 ms. |
| 0x30 | At start: write 0x00010100. |
| 0x34 | Transmit control. Bit 0 enable; bit 1 stop request (the chip clears it when stopped); bit 2 "look at the ring" (kick). Modify with read-OR-write. |
| 0x38 | Receive control. Same bits as 0x34. |
| 0x3C / 0x44 | Transmit ring base (two registers; write 0x44 first, then 0x3C, both = ring physical address). |
| 0x40 / 0x48 | Receive ring base (write 0x48 first, then 0x40, both = ring physical address). |
| 0x50 | Interrupt status. Write the read value back to clear. Bits 1 and 5 mean the receiver ran out of descriptors. |
| 0x54 | Interrupt mask, 1 = masked. 0x7FFFFF masks every source. |
| 0x74 | At start: write 0x00303277. |
| 0x9C | At start, right before enabling: clear bit 6, then set bit 6. |
| 0xAC | MAC reset: write 9, then wait about 2 ms. The link stays up across it. |
| 0x110, 0x114 | Statistics, read once at start (read clears). |
| 0x204 | Interrupt coalescing: write 0. |

## Sequences

**Stop** (also the first thing at start: the previous OS leaves the chip running with rings in memory that now belongs
to Windows): mask = 0x7FFFFF; coalescing = 0; transmit control |= bit 1, wait until bit 1 reads 0 (1 ms steps, ≤ 200 ms);
same for receive control.

**Start**: stop; MAC reset (0xAC = 9, ~2 ms); 0x08 |= 0x07597C00; 0x0C &= ~bit 7; 0x74 = 0x303277;
0x10 = (0x10 & 0xFFFFFF6E) | 0x81; 0x30 = 0x10100; MAC high/low; coalescing 0; read 0x110 and 0x114; fill both rings
(below); store fence; ring base registers (B then A, TX then RX); 0x9C bit 6 off then on; transmit control |= 1;
receive control |= 1; clear interrupt status; mask = 0x7FFFFF (the driver polls; no interrupt is used); multicast
setup: rx config bit 4 off (not promiscuous), clear the hash table, then set all 256 entries (so every multicast frame
is received and NDIS filters).

**No PHY bring-up.** The PHY is used as the previous OS (Linux) left it: link negotiated, calibrated. The driver
depends on Linux having brought the port up once since power-on.

## Descriptor rings (16-byte descriptors = 4 little-endian dwords w0..w3)

**Receive ring**: 1024 descriptors. Each: w0 = buffer size 0x600 (bit 30 also set on the last descriptor: wrap),
w1 = buffer physical address (one 0x600-byte buffer per descriptor), w2 = 0, w3 = 0. Write w1-w3 before w0. When a
frame has arrived the chip sets w0 bit 31 and puts the length in w0 bits 0-10. Frames shorter than 60 or longer than
0x600 are bad: drop them. Lengths above 1514 are clamped to 1514 when indicating. Give a descriptor back by writing
w2 = w3 = 0, then w0 = size (+ wrap bit on the last). After taking any frames, or when interrupt status shows bit 1 or
bit 5, set receive control bit 2 so the receiver looks at the ring again.

**Transmit ring**: 256 descriptors. Initially every descriptor: w0 = 0x80000000 ("done/free"; bit 30 also on the last),
w1 = 0, w2 = 0xFFFF0000, w3 = 0. To send one frame: copy it into DMA memory (128-byte aligned slots), pad to at least
60 bytes with zeros, max 0x600 bytes; fill the next descriptor: w1 = buffer address, w2 = 4, w3 = 0, then
w0 = length | 0x30000000 (| 0x40000000 on the last descriptor); store fence; transmit control |= bit 2. Completion: the
chip sets w0 bit 31 again; a descriptor is reclaimed when w0 bit 31 is set and w2 is below 0xFFFF0000, then mark it by
setting w2's top 16 bits (w2 |= 0xFFFF0000). Ring full = next index equals the oldest unreclaimed one.

## Windows side (behaviour of the current driver, the project's own design)

- NDIS 6.30 deserialized miniport, NDIS_WDM, interface type internal, medium 802.3, MTU 1500, max link 1 Gb/s
  full duplex, reported link speed 1 Gb/s when up. Packet filters accepted: directed, multicast, all-multicast,
  broadcast, promiscuous (all are effectively "receive all multicast"; no chip reprogramming per filter).
  Multicast list size 32 (accepted, ignored). Statistics: xmit OK, rcv OK.
- One kernel thread does everything: link check every ~200 ms (register 0x04 bit 0 → link indications), receive up to
  256 frames per pass, send queued frames, reclaim TX. Idle: short spins, then 1 ms waits; timer resolution 1 ms
  while running. Sends from NDIS are queued under a spin lock and the thread is woken.
- Receive indication: copy into one driver-owned buffer and indicate with NDIS_RECEIVE_FLAGS_RESOURCES.
- Optional diagnostics: DWORD/string values under `HKLM\SYSTEM\CurrentControlSet\Services\p5eth` (State, Stage,
  Link, RxOk, TxOk, ...), written at PASSIVE_LEVEL.
- Halt: stop the thread, stop the chip (sequence above), unmap, restore the root port if it was changed.

## Build facts (added after review, 2026-11-22)

- Windows 10 refuses a kernel driver whose PE DllCharacteristics include TERMINAL_SERVER_AWARE (0x8000) or
  HIGH_ENTROPY_VA (0x20) (load fails with 0xC0000018). With lld-link: `/highentropyva:no /tsaware:no`. Expected value
  0x140 (DYNAMIC_BASE | NX_COMPAT).
