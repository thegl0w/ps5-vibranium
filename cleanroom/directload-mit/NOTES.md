# CyanDirectLoad (MIT clean-room rewrite)

Drop-in replacement for the GPU microcode direct-load component used by `ps5gpu/tool/stage2.cpp`. Written from
`../spec-directload.md` only, plus Linux amdgpu (MIT) for background and `stage2.cpp` for the interface. Nothing
under `MetalCyan/` or the other excluded paths was opened.

## Files

- `CyanDirectLoad.hpp` — the interface the spec lists (namespace `CyanDirectLoad`: `Io`, `Kind`, `Image`, `parse`,
  `CpImages`, `Store`, `kStoreAlign`, `storeBytesNeeded`, `Status`, `statusName`, `Snapshot`, `snapshot`, `Report`,
  `kHaltTimeoutUs`, `kIcacheTimeoutUs`, `quiesce`, `disableGpaMode`, `loadRlc`, `stageCp`, `loadCp`, `storeIntact`,
  `loadSdma`) plus a `Reg` sub-namespace holding every register offset and bit used. `stage2.cpp` uses no `Reg`
  names, so the names there are my own (CamelCase; no `mm*` names, so no clash with the amdgpu headers that
  `stage2.cpp` includes after this one).
- `CyanDirectLoad.cpp` — implementation. No OS calls, no allocation; all register access through `Io`.
- `test_directload.cpp` — host test.

## Design

- Firmware: little-endian reads byte by byte (no unaligned loads), 64-bit arithmetic for every bounds check, so a
  hostile header cannot overflow. Every kind checks the PSP container magic; RLC/SDMA/PFP/CE/ME require inner size ==
  payload − 0x200; MEC walks both containers and requires exact sizes and jt_offset×4 == 0x200 + first inner size.
- CP jump tables must be non-empty: `parse()` refuses a CpGfx file with jt_size 0 and a MEC file whose second
  container's inner size is 0, and a CP `Image` with `jt == nullptr` or `jtDwords == 0` is not usable (`stageCp` /
  `loadCp` → `BadImage` with no register access, `storeIntact` → false). RLC and SDMA images have no jump table.
- One CP-engine routine (`loadCpEngine`) parameterised by the engine's register set and two flags: write IC_BASE_CNTL
  back (PFP and MEC only) and keep the VMID field (MEC only).
- Validation (images, store alignment/size) happens before the first register access in `loadCp`, `loadRlc`,
  `loadSdma`; a bad input produces zero register traffic and `Report.step` 0. A null `cpu`, a GPU address not 4 KiB
  aligned, or too few bytes all give `StoreTooSmall`; `StoreMismatch` means a copy into the store did not read back.
- Polling (CP_STAT halt wait, IC_OP_CNTL invalidate wait): up to `timeout` rounds of read / stop if done / delay 1 µs.
  The reported wait is the number of rounds that did not see the value, i.e. 0 if the first read already shows it and
  `timeout` when it never came (then `timeout` reads and `timeout` delays). A CP halt timeout is reported and the load
  carries on; an instruction-cache timeout ends `loadCp` with `IcacheTimeout` and the engine's step number.
- `parse()` zeroes `out` first, so it is all zero after a failure.
- Registers touched: CP_ME_CNTL, CP_STAT (read), CP_MEC_CNTL, the four CP engines' IC_OP_CNTL / IC_BASE_CNTL /
  IC_BASE_LO / IC_BASE_HI / UCODE_ADDR / UCODE_DATA, RLC_GPM_UCODE_ADDR/DATA, CPG/CPC_PSP_DEBUG, SDMA0/1 CNTL,
  GFX_RB_CNTL, GFX_IB_CNTL, F32_CNTL, UCODE_ADDR/DATA, GRBM_SOFT_RESET (SDMA bits only, when asked). `snapshot` adds
  reads of RLC_CNTL, GRBM_STATUS and SDMA STATUS. No PSP, SMU or bus reset.

## Spec gaps

Nine questions in `../directload-questions.md`, answered by the spec author in `../directload-answers.md`
(2026-11-21). Applied: (1) misaligned store / null cpu → `StoreTooSmall`; (2) step 0 for BadImage in loadRlc and
loadSdma; (3) poll bound = `timeout` rounds, wait = round count, halt timeout non-fatal, icache timeout fatal;
(8) `out` zeroed at the start of parse. Confirmed unchanged: (4) soft-reset clear value from the read-back,
(5) MEC jump-table size must be a multiple of 4, (6) magic checked on every first container and on MEC's second,
(7) RLC/SDMA jt fields null/0/0, (9) loadCp re-copies each image and never writes the padding.

Spec author review (2026-11-21): registers, bits, order, PFP-only IC_BASE_CNTL write, MEC VMID kept, SDMA strides and
the soft-reset sequence match. One added fact, applied: an empty CP jump table is refused (see Design).

One reading of mine remains: on success the reported wait counts the rounds before the one that saw the value
(first read done → 0). The answer says "the iteration count"; if the original counts the successful round too, the
report would be 1 higher. This affects only the printed number, not the register traffic.

## Results (2026-11-21)

- Host test: `clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -I. test_directload.cpp CyanDirectLoad.cpp`
  → **132 checks, 0 failures** (with the empty-jump-table rule; 102 after the answers, 93 at first). Covers: parse good/bad for every kind (MEC two-container layout, both magics, jt_offset
  mismatch, size slack in either direction, header size/offset/overflow/alignment, minimum sizes), snapshot (28 reads,
  no writes), disableGpaMode, quiesce with and without SDMA reset (exact access lists incl. delays), halt timeout
  (100000 reads and 100000 delays, carries on), loadRlc, loadSdma (incl. the +0x600 / +0x20 strides), stageCp / storeIntact / store
  layout, full loadCp access list with a high GPU address, store errors (too small, misaligned, null cpu → all
  `StoreTooSmall`, step 0) and image errors with no register traffic, icache timeouts on PFP (exactly 50000 poll
  reads) and on MEC, `out` zeroed after a failed parse, empty jump tables (CpGfx jt_size 0, MEC second inner size 0,
  and jtDwords 0 / jt null on each of the four CP images) refused, one-dword jump tables accepted. Expected register numbers in the test are retyped from the spec table, not taken from
  the header.
- Mutation check: writing CE/ME IC_BASE_CNTL, a wrong SDMA1 ucode stride, a wrong soft-reset clear value, one
  poll round too many, not zeroing `out` in parse, and dropping any of the three empty-jump-table checks are each
  caught.
- `x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only -I cleanroom/directload-mit -I build/gpusrc/.../amd/include -I .../amdgpu ps5gpu/tool/stage2.cpp`
  → compiles (one pre-existing -Wsign-compare warning in stage2.cpp's ringdump, unrelated).
  `CyanDirectLoad.cpp` also compiles with mingw at `-O2 -Wall -Wextra -Werror`.
- Not tested on the console (by design). To integrate, point build.sh's `-I"$MC"` and `"$MC/CyanDirectLoad.cpp"` at
  this folder.
