# Clean-room spec: Cyan Skillfish GPU microcode "direct load" over MMIO (target: MIT replacement for CyanDirectLoad)

Facts only: register offsets, bits, file format, order of operations, and the interface our caller needs. No code.
Written 2026-11-21 by the session that has read the current implementation (MetalCyan's `CyanDirectLoad`, licensed
"Thou Shalt Not Profit"); the implementer must not open it (see the brief). Public background that may be read
freely: Linux's `drivers/gpu/drm/amd/amdgpu` (MIT licensed) — `gfx_v10_0.c` (disable_gpa_mode, rlc_load_microcode,
cp_gfx_load_microcode, cp_compute_load_microcode, cp_gfx_enable/cp_compute_enable), `sdma_v5_0.c` (load_microcode,
enable, ctx_switch_enable, soft_reset), `amdgpu_ucode.h` (firmware headers), `gc_10_1_0_offset.h`/`_sh_mask.h`.
NOT allowed: the ps5-linux kernel patches (GPL) — the PS5-specific facts from them are below.

## Context

The GPU (AMD Cyan Skillfish, GC 10.1.3, SDMA 5.0.1, PCI 1002:13FC) on the PS5 loads its microcode without the PSP:
the driver writes it over MMIO ("direct load"). Our Windows tool `ps5gpu2.exe` (`windows-handoff/ps5gpu/tool/stage2.cpp`,
our own MIT code, which you may read) does the bring-up and calls this component. Register access goes through
callbacks supplied by the caller; this component has no OS dependency.

## Interface the caller needs (namespace `CyanDirectLoad`, header `CyanDirectLoad.hpp`)

Keep these names and shapes so `stage2.cpp` compiles unchanged (read `stage2.cpp` to see exactly how each is used):
- `struct Io { void* ctx; uint32_t (*read)(void*, uint32_t reg); void (*write)(void*, uint32_t reg, uint32_t value);
  void (*delayUs)(void*, uint32_t us); }` — `reg` is an absolute dword offset into the register BAR (IP base included).
- `enum class Kind : uint8_t { Rlc, Sdma, CpGfx, CpMec }`.
- `struct Image { const uint8_t* code; uint32_t codeBytes; const uint8_t* jt; uint32_t jtDwords; uint32_t jtDwordsHdr;
  uint32_t version; }` and `bool parse(const uint8_t* file, size_t size, Kind, Image& out)`.
- `struct CpImages { Image pfp, ce, me, mec; }`; `struct Store { uint8_t* cpu; uint64_t gpu; uint64_t bytes; }`;
  `constexpr uint32_t kStoreAlign = 0x1000`; `uint64_t storeBytesNeeded(const CpImages&)`.
- `enum class Status : uint8_t { Ok, BadImage, StoreTooSmall, StoreMismatch, IcacheTimeout }`;
  `const char* statusName(Status)`.
- `struct Snapshot` with fields (all uint32_t): rlcCntl, rlcGpmUcodeAddr, grbmStatus, cpMeCntl, cpMecCntl, cpStat,
  cpgPspDebug, cpcPspDebug, pfpIcBaseLo/Hi/Cntl, ceIcBaseLo/Hi/Cntl, meIcBaseLo/Hi/Cntl, cpcIcBaseLo/Hi/Cntl, and
  arrays [2] sdmaF32Cntl, sdmaCntl, sdmaStatus, sdmaRbCntl; `void snapshot(const Io&, Snapshot&)` reads them.
- `struct Report { Status status; uint32_t step; uint32_t cpStatAfterHalt; uint32_t haltWaitUs; uint32_t
  icacheWaitUs[4]; uint32_t dataWrites; }`; `constexpr uint32_t kHaltTimeoutUs = 100000, kIcacheTimeoutUs = 50000`.
- `void quiesce(const Io&, bool resetSdma)`, `void disableGpaMode(const Io&)`, `Report loadRlc(const Io&, const Image&)`,
  `Status stageCp(const CpImages&, const Store&)`, `Report loadCp(const Io&, const CpImages&, const Store&)`,
  `bool storeIntact(const CpImages&, const Store&)`, `Report loadSdma(const Io&, const Image& sdma0, const Image& sdma1)`.
- Any register constants `stage2.cpp` uses from a `Reg` sub-namespace: check `stage2.cpp` and provide those names.

## Register map (absolute dword offsets; IP bases GC0 = 0x1260, GC1 = 0xA000, as Navi 10)

| Name | Offset |
|---|---|
| GRBM_STATUS | GC0 + 0x0DA4 |
| GRBM_SOFT_RESET | GC0 + 0x0DA8 (SDMA0 reset bit 0x00800000, SDMA1 the next bit) |
| CP_MEC_CNTL | GC0 + 0x0E2D (ME1 halt 0x40000000, ME2 halt 0x10000000) |
| CP_STAT | GC0 + 0x0F40 |
| CP_ME_CNTL | GC0 + 0x0F56 (CE halt 0x01000000, PFP halt 0x04000000, ME halt 0x10000000) |
| RLC_CNTL | GC1 + 0x4C00 |
| RLC_GPM_UCODE_ADDR / DATA | GC1 + 0x5B61 / 0x5B62 (load start address 0x2000) |
| CPG_PSP_DEBUG / CPC_PSP_DEBUG | GC1 + 0x5C10 / 0x5C11 (GPA_OVERRIDE bit 0x8) |
| PFP: IC_BASE_LO/HI/CNTL, IC_OP_CNTL, UCODE_ADDR/DATA | GC1 + 0x5840/0x5841/0x5842, 0x5843, 0x5814/0x5815 |
| ME: same set | GC1 + 0x5844/0x5845/0x5846, 0x5847, 0x5816/0x5817 |
| CE: same set | GC1 + 0x5848/0x5849/0x584A, 0x584B, 0x5818/0x5819 |
| MEC (CPC_IC_*, MEC_ME1_UCODE_*) | GC1 + 0x584C/0x584D/0x584E, 0x584F, 0x581A/0x581B |
| SDMA0_CNTL / STATUS / F32_CNTL / GFX_RB_CNTL / GFX_IB_CNTL | GC0 + 0x001C / 0x0025 / 0x002A / 0x0080 / 0x008A |
| SDMA0_UCODE_ADDR / DATA | GC1 + 0x5880 / 0x5881 |
| SDMA1 | SDMA0's register + 0x600; SDMA1's ucode pair = SDMA0's + 0x20 |

Bits: IC_OP_CNTL invalidate 0x1, invalidate-complete 0x2. IC_BASE_CNTL: VMID field 0xF, ADDRESS_CLAMP 0x10,
EXE_DISABLE 0x00800000, CACHE_POLICY 0x03000000. SDMA F32_CNTL halt 0x1; SDMA_CNTL AUTO_CTXSW 0x00040000;
RB/IB enable 0x1.

## Firmware files (linux-firmware `cyan_skillfish2_{rlc,sdma,sdma1,pfp,ce,me,mec}.bin`, zstd-decompressed by the caller)

- amdgpu common header (little-endian dwords): +0x00 total size in bytes (must equal the file size), +0x10
  ucode_version, +0x14 ucode payload size, +0x18 payload offset; gfx header: +0x24 jt_offset (dwords), +0x28 jt_size
  (dwords). File must be at least 0x2C bytes; payload must lie inside the file, be a multiple of 4 and larger than
  0x200.
- **PS5 fact:** the payload is wrapped in a PSP container: a 0x100-byte header (magic "$PS1" = dword 0x31535024 at
  container +0x10, inner size dword at +0x14), the inner code, then a 0x100-byte signature. Direct load uses only the
  inner code: start = payload + 0x100, bytes = payload size − 0x200. For RLC, SDMA, PFP, CE, ME the inner size must
  equal that. `version` = ucode_version.
- CP gfx (PFP/CE/ME): the jump table is `jt_size` dwords at inner-code + jt_offset×4; it must fit in the code.
- **MEC:** the payload holds two containers back to back: [hdr 0x100][MEC code: inner size bytes][sig 0x100] then
  [hdr 0x100][jump table][sig 0x100]; the second container's magic must be "$PS1" too; the second starts at
  0x200 + first inner size, which must equal jt_offset×4; total must match exactly; jump table = second container's
  inner bytes (its inner size / 4 dwords). `code`/`codeBytes` = from payload + 0x100, payload size − 0x200 bytes (i.e.
  everything after the first header, minus 0x200, as the CP copy uses). `jtDwordsHdr` keeps the header's jt_size.

## Sequences

- **snapshot**: read the registers listed in Snapshot (SDMA ones for both instances).
- **quiesce(resetSdma)** — make an inherited running GPU safe before reprogramming:
  1. CP gfx halt: CP_ME_CNTL |= CE|PFP|ME halt; poll CP_STAT until 0, 1 µs steps, up to kHaltTimeoutUs.
  2. CP compute halt: CP_MEC_CNTL = ME1|ME2 halt (plain write), wait 50 µs.
  3. SDMA context switch off: each instance SDMA_CNTL &= ~AUTO_CTXSW.
  4. SDMA stop and halt: for both instances clear RB enable in GFX_RB_CNTL and IB enable in GFX_IB_CNTL; then for both
     set F32_CNTL halt.
  5. If resetSdma: for instance 0 then 1: GRBM_SOFT_RESET |= that instance's bit, read back, wait 50 µs, clear the
     bit, read back; then step 4 again. RLC untouched.
- **disableGpaMode**: CPC_PSP_DEBUG |= 0x8, then CPG_PSP_DEBUG |= 0x8.
- **loadRlc**: (caller has stopped the RLC.) RLC_GPM_UCODE_ADDR = 0x2000; write every code dword to
  RLC_GPM_UCODE_DATA; RLC_GPM_UCODE_ADDR = version. dataWrites counts dwords written.
- **CP store layout**: PFP at offset 0, then CE, ME, MEC, each starting on a 4 KiB boundary (sizes rounded up to
  4 KiB). storeBytesNeeded = sum of the rounded sizes. Store gpu address must be 4 KiB aligned and big enough.
  stageCp copies all four and compares (memcmp) without touching registers. storeIntact compares again.
- **loadCp**: validate; halt CP gfx as above (record CP_STAT result and wait in the report); then for PFP (step 1),
  CE (2), ME (3): copy the image into the store and verify; IC_OP_CNTL |= invalidate, poll for invalidate-complete
  (1 µs steps, up to kIcacheTimeoutUs; timeout → IcacheTimeout); take IC_BASE_CNTL, clear the VMID field, clear
  CACHE_POLICY and EXE_DISABLE, set ADDRESS_CLAMP — write it back for PFP only (for CE and ME the value is computed
  but not written); IC_BASE_LO = low 32 bits of (store gpu + offset) & 0xFFFFF000, IC_BASE_HI = high 32 bits;
  UCODE_ADDR = 0; write the jump table dwords to UCODE_DATA; UCODE_ADDR = version. Then CP compute halt (as in
  quiesce step 2) and MEC (step 4) the same way, except its IC_BASE_CNTL keeps its VMID field and IS written. Leave
  everything halted; step 0 on success.
- **loadSdma**: SDMA stop and halt (quiesce step 4); for instance 0 and 1 (step = instance + 1): UCODE_ADDR = 0, every
  code dword to UCODE_DATA, UCODE_ADDR = version (instance 1 uses the +0x20 pair). Leave halted.
