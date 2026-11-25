# ps5wddm: what WDDM 2.0 (GpuMmu) asks, and what we answer

Phase 0 of `windows-handoff/WDDM2-DX12-PLAN.md`, written 2026-11-23 (Opus). Sources:

- **H** = the 28000 WDK headers (`build/wdk/x-wdk.x64/.../shared/d3dkmddi.h`, `km/dispmprt.h`, sdk `shared/d3dukmdt.h`).
  The driver is built with `DXGKDDI_INTERFACE_VERSION=0x5023` (= WDDM 2.0), so the structures compile in their 2.0 shape.
- **D** = Microsoft's public driver documentation (learn.microsoft.com, read 2026-11-23; quoted where it decides something).
- **B** = Windows' own binaries of 19041.2965 (22H2 ISO in ~/Downloads, `install.wim` index 1) with Microsoft's public
  symbols: `dxgkrnl.sys`, `dxgmms2.sys`, `D3D12Core.dll`, `d3d10warp.dll` (scratchpad only, nothing kept in the tree).

Each line is marked **documented** (H or D say so), **binary** (seen in B) or **guess** (to be proven in phase 1).

## Version to register

**WDDM 2.0** (`DXGKDDI_INTERFACE_VERSION_WDDM2_0`, `DXGKDDI_WDDMv2`). Direct3D 12 needs a WDDM 2.0 kernel driver with GPU
virtual addressing (D3D12's fences are monitored fences, which live at a GPU virtual address); nothing in 2.1+ is needed
by D3D12 on 19044 (2.1 adds 64 KB system pages, move paging, VPR; 2.2 paravirtualisation; 2.3+ hardware queues). — guess
for "nothing above 2.0 needed", documented for the rest. Going higher only adds required entry points.

## Why the 2026-11-20 run died (reading, not proven)

That run registered `f.Version = WDDM2_0` but filled the 1.3 table: every 2.0 pointer (`DxgkDdiCreateProcess`,
`SubmitCommandVirtual`, `SetRootPageTable`, ...) and the 1.3 ones we never filled (`CalibrateGpuClock`,
`FormatHistoryBuffer`, `SetPowerPState`, `ControlInterrupt2`) were NULL, and `DRIVERCAPS` said no virtual addressing.
A call through a NULL pointer at kernel level is an immediate bug check before any trace line can be flushed, which fits
"died right after the segment questions, nothing in the trace". **Rule for phase 1: in 2.0 mode every pointer of the 2.0
table is non-NULL**, each a traced stub at least.

## Physical and virtual contexts (the question the plan left open)

- documented (D, pfnSubmitCommandCb): virtual contexts use `pfnSubmitCommandCb`; "Contexts that operate in legacy patch
  mode must continue to use the old pfnRenderCb." D (GPU virtual memory in WDDM 2.0): "Individual engines of a GPU can
  operate in either physical or virtual mode"; "In physical mode, the scheduling model remains the same as it is with
  WDDM v1.x."
- documented (H): `DXGK_CREATECONTEXTFLAGS.VirtualAddressing` tells `DxgkDdiCreateContext` which kind it creates.
  Contexts made with `pfnCreateContextCb` (no Virtual) are physical; the D3D12 runtime also offers `pfnCreateContextCb`.
- So: **our Direct3D 10 driver (Mesa) keeps its physical context and its `pfnRenderCb` "P5CP/P5C2" copy commands**,
  and presents keep going through `DxgkDdiPresent` -> `DxgkDdiPatch` -> `DxgkDdiSubmitCommand` exactly as in 1.3.
  guess until phase 1 shows it: that a GpuMmu adapter accepts physical contexts on our one node. Fallback if not: the
  D3D10 driver creates a virtual context and submits its copy command with `pfnSubmitCommandCb`, private data = the same Op.
- The new `ps5d3d12.dll` creates a virtual context only if the runtime demands one; its presents use a physical context.

## Driver entry points (the 2.0 table in `DRIVER_INITIALIZATION_DATA`)

| Entry point | In 2.0 | What ps5wddm does in 2.0 mode | Mark |
|---|---|---|---|
| `DxgkDdiCreateProcess` | new; called per process that opens the adapter | allocate a small `Proc` (root page table address, refcount), hand it back as `hKmdProcess`; never fails | documented (H, D) |
| `DxgkDdiDestroyProcess` | new | free the `Proc` | documented (H) |
| `DxgkDdiSetRootPageTable` | new; tells a context where its root table is now | store `Address`/`NumEntries` in the context (used only by our own page-table walker, see fences) | documented (H, D) |
| `DxgkDdiGetRootPageTableSize` | new; only called when `PageTableLevelCount == 2` (D) | return `NumberOfPte * 8` rounded up to 4 KB; not called with our 4 levels, filled anyway | documented (D) |
| `DxgkDdiSubmitCommandVirtual` | new; DMA buffers of virtual contexts (and, guess, paging buffers of a GpuMmu adapter) | same as `SubmitCommand`: run the Ops in the private data (`DmaBufferPrivateDataSize` bytes from the start), complete `SubmissionFenceId` through the engine thread; a buffer without our magic just completes | documented (H, D) / guess for paging |
| `DxgkDdiRenderGdi` | new; GDI acceleration for virtual contexts | not used: `SupportKernelModeCommandBuffer` stays 0 in 2.0 mode (GdiAccel forced off); stub writes an empty Op | documented (D) |
| `DxgkDdiMapCpuHostAperture` / `Unmap...` | new; only for segments with `SupportsCpuHostAperture` | we don't set that flag; stubs return `STATUS_NOT_SUPPORTED` | documented (H) |
| `DxgkDdiCheckMultiPlaneOverlaySupport2`, `SetVidPnSourceAddressWithMultiPlaneOverlay2` | new; only with `MaxOverlayPlanes > 0` | report "not supported" / forward to `SetVidPnSourceAddress` | documented (H) |
| `DxgkDdiPowerRuntimeSetDeviceHandle` | new | stub, success | guess |
| `DxgkDdiSetStablePowerState` | new; `ID3D12Device::SetStablePowerState` lands here | success, nothing done (clocks stay as Linux left them; SAFETY: no SMU messages) | documented (H) |
| `DxgkDdiSetVideoProtectedRegion` | new; VPR only | `STATUS_NOT_SUPPORTED` | documented (H) |
| `DxgkDdiCalibrateGpuClock` (1.3) | asked by D3D12 for `GetClockCalibration`/timestamps | GPU clock = QPC: `GpuFrequency = CpuFrequency = KeQueryPerformanceCounter` frequency, both counters from QPC (our timestamps are CPU time anyway) | documented (H) / guess that it is called |
| `DxgkDdiFormatHistoryBuffer` (1.3) | ETW only | `STATUS_SUCCESS`, nothing written (we keep no history buffers) | documented (H) |
| `DxgkDdiSetPowerPState`, `ControlInterrupt2` (1.3) | 1.3 optional | stubs: success / forward to `ControlInterrupt` | guess |
| `DxgkDdiRender`, `DxgkDdiPatch`, `DxgkDdiPresent`, `DxgkDdiSubmitCommand` | still called for physical contexts and presents | unchanged | documented (D: legacy patch mode) |
| `DxgkDdiRenderKm` | replaced by `RenderGdi` for virtual contexts | unchanged, unused (GdiAccel off) | documented (D) |
| `DxgkDdiBuildPagingBuffer` | gains new operations (below) | old operations unchanged, new ones handled | documented (H) |
| `DxgkDdiCreateDevice` | gains `hKmdProcess` (in) | remember it in nothing (device is still the adapter) | documented (H) |
| `DxgkDdiCreateContext` | gains `Flags.VirtualAddressing` | trace it; same answer for both kinds (one node, DMA buffer 64 KB, private data one Op per 16 bytes); virtual contexts get no allocation/patch lists | documented (H) |
| `DxgkDdiCreateAllocation` | 2.0 adds `DXGK_ALLOCATIONINFOFLAGS_WDDM2_0` and `PhysicalAdapterIndex`, GPU VA is given by the UMD/runtime later | same answer as 1.3 for our P5 allocations; allocations made by D3D12 (no `P5A_MAGIC`) are accepted as plain buffers in segment 1 or system memory (see "foreign allocations") | guess |
| `DxgkDdiEscape` | unchanged | new escape `P5E_FENCE_READ` (fences, below), 2.0 only | ours |

Every other 1.3 entry point: as now. Everything new is behind `gWddm2`.

## Adapter questions (`DxgkDdiQueryAdapterInfo`)

| Type | Asked when | Our answer in 2.0 mode | Mark |
|---|---|---|---|
| `DRIVERCAPS` (1) | always | as 1.3 plus `WDDMVersion = DXGKDDI_WDDMv2`, `MemoryManagementCaps.VirtualAddressingSupported = 1`, `GpuMmuSupported = 1`, `IoMmuSupported = 0` (both may not be set, D), `ReplicateGdiContent = 0`, `NonCpuVisiblePrimary = 0`; `InternalGpuVirtualAddressRange{Start,End} = 0` (we reserve none); `SupportPerEngineTDR = 1` (already 1.3 behaviour via ResetEngine); GDI acceleration off | documented (H, D) |
| `QUERYSEGMENT4` (11) | 2.0 asks this one | one memory segment, as now (`CpuVisible`, base = GPU address of the carve-out range, CPU address = BAR window) plus 2.0 flags: `LocalBudgetGroup = 1` (H: replaces the deprecated `ApplicationTarget`), `Use64KBPages = 0`, `SupportsCpuHostAperture = 0`, `PreservedDuringStandby/Hibernate = 0`. **No aperture segment in 2.0 mode** (system memory is "segment 0" in GpuMmu, D: "The value zero is reserved for system memory") | documented (H, D) / guess: no aperture needed |
| `GPUMMUCAPS` (13) | 2.0 GpuMmu | `PageTableUpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL`; `VirtualAddressBitCount = 48`; `PageTableLevelCount = 4`; `LeafPageTableSizeFor64KPagesInBytes = 256` (only used with dual PTEs, which we don't claim); flags all 0 except `ExplicitPageTableInvalidation = 0` (no TLB to flush) and `CacheCoherentMemorySupported = 1` (system pages are reached by the CPU only); `LegacyBehaviors = 0` | documented fields (H), values ours. D: level count minimum is 2; "When page directories are located in a local GPU memory segment, the update mode cannot be set to CPU_VIRTUAL" (ours are in system memory, so allowed) |
| `PAGETABLELEVELDESC` (14) | once per level (input = level index) | every level: `PageTableIndexBitCount = 9`, `PageTableSegmentId = 0` (system memory), `PagingProcessPageTableSegmentId = 0`, `PageTableSizeInBytes = 4096`, `PageTableAlignmentInBytes = 4096`. 12 + 4 x 9 = 48 bits | documented (D: system memory tables "cannot be more than 4 KB"; size a multiple of the CPU page) |
| `PHYSICALADAPTERCAPS` (15) | 2.0 | as now, plus `Flags.GpuMmuSupported = 1`; `NumExecutionNodes = 1`, `PagingNodeIndex = 0` | documented (H) |
| `HISTORYBUFFERPRECISION` (10) | asked 2026-11-20 | as now: 0 bits per engine | binary (trace 2026-11-20) |
| `DISPLAY_DRIVERCAPS_EXTENSION` (16) | asked 2026-11-20 | as now: all 0 | binary (trace) |
| `SEGMENTMEMORYSTATE` (12), `UEFIFRAMEBUFFERRANGES` (18), others | maybe | `STATUS_NOT_SUPPORTED`, traced | guess |
| `UMDRIVERPRIVATE` (0) | always | as now (`P5AdapterInfo` v4) plus a field `wddm = 20` so the user-mode drivers know the mode | ours |

## Paging operations (`DxgkDdiBuildPagingBuffer`)

All are done by our CPU engine thread in submission order (as today), or immediately where Windows says so.

| Operation | Meaning | What we do | Mark |
|---|---|---|---|
| `TRANSFER` (0), `FILL` (1) | move/fill allocation content between system memory and our segment | unchanged (the header's "WDDMv1 Only" means the old Vista-era "v2", see that `VIRTUAL_TRANSFER` is guarded by `WIN7`) | documented (H) |
| `DISCARD_CONTENT` (2) | | nothing, as now | documented |
| `MAP_APERTURE_SEGMENT` / `UNMAP` (5, 6) | aperture segments | not reached in 2.0 mode (no aperture); code kept | documented |
| `INIT_CONTEXT_RESOURCE` (10) | context allocations | nothing (we ask for none) | documented |
| `UPDATE_PAGE_TABLE` (11) | write `NumPageTableEntries` `DXGK_PTE`s (16 bytes each, VidMm's format) at `StartIndex` of the table `PageTableAddress`; `UpdateMode` is CPU_VIRTUAL for us, so `PageTableAddress.CpuVirtual` is a kernel mapping of the 4 KB table. D: during paging-process initialization "pDmaBuffer is set to NULL. In this case the driver must update page tables immediately." | translate each `DXGK_PTE` into our 8-byte entry: `PageAddress << 12 | Valid | ReadOnly<<1 | Segment<<2 (5 bits) | Zero<<7`; non-leaf levels the same with `PageTableAddress`. Written **immediately on the CPU** in every case (VidMm only issues the update once the table is not in use, and nothing but our own walker reads it); `Flags.Repeat` = one entry repeated; counted in the trace. Nothing reaches the GPU | documented (H, D) / guess: immediate write is fine outside initialization |
| `FLUSH_TLB` (12) | invalidate after a root change | nothing (no GPU uses these tables); counted | documented |
| `UPDATE_CONTEXT_ALLOCATION` (13) | | nothing | documented |
| `COPY_PAGE_TABLE_ENTRIES` (14) | copy PTE ranges between tables (GPU VA of tables) | done on the CPU through our table map (see walker); counted | documented (H) / guess for the address kind |
| `NOTIFY_RESIDENCY` (15) | allocation became resident / evicted at `PhysicalAddress` | nothing; counted | documented |
| `VIRTUAL_TRANSFER` (8) / `VIRTUAL_FILL` (9) | transfers by GPU VA (only if the paging node runs virtual) | resolve both VAs with our walker and copy/fill on the CPU; counted. Expected unused (we answer CPU_VIRTUAL, and our paging node is physical) | documented (H) / guess unused |
| `SIGNAL_MONITORED_FENCE` (16) | 2.2+ | not reached at 2.0 | documented |

## Fences (what makes Direct3D 12 work without the GPU knowing about Windows)

- documented (H, d3d12umddi `D3D12DDI_FENCE`): the D3D12 runtime creates an `ID3D12Fence` as a dxgkrnl monitored fence and
  gives the user-mode driver only its **GPU virtual addresses** (`FenceValue.BaseAddress`, `FenceMonitoredValue`), no
  kernel handle. Queue `Signal`/`Wait` reach the user-mode driver (`pfnSignalFence`, `pfnWaitForFence`).
- Our GPU work runs outside dxgkrnl (RADV -> ps5gpu.sys), so the fence must be written from the CPU when the work is done.
  `DXGK_INTERRUPT_MONITORED_FENCE_SIGNALED` exists only from 2.2 (H), so at 2.0 the wake-up has to ride on an ordinary
  `DMA_COMPLETED` (guess: dxgkrnl re-checks monitored fences on every completion; that is how a 2.0 GPU's fence write
  becomes visible).
- **Write (queue Signal):** the D3D12 driver's queue thread, once RADV's work up to the signal is done, submits a tiny
  DMA buffer on its virtual context (`pfnSubmitCommandCb`, through `SubmitCommandVirtual`) whose private data is one
  `OP_FENCE {gpu va, value}`. The engine thread runs it in order: our page-table walker finds the system page behind the
  VA in that context's tables (root from `SetRootPageTable`), maps 8 bytes (`MmMapIoSpaceEx`), writes the value, then the
  submission completes with `DMA_COMPLETED`. This is the GPU's own model (write, then interrupt), done by the CPU. guess.
- **Read (queue Wait on a fence another queue or the CPU signals):** escape `P5E_FENCE_READ {gpu va}` -> the same walker
  reads the value. The queue thread polls it (back-off 0.1 -> 2 ms) only while a wait is pending. guess, cost measured in
  phase 3.
- The page-table walker is the only reader of our page tables. It refuses any VA that is not mapped, not in segment 0
  (system memory) or the memory segment, or crosses a page.

## Foreign allocations (made by the D3D12 runtime/driver, not by our P5 path)

- The D3D12 driver allocates through Windows only what Windows must see: swap-chain buffers (shared with DWM), and small
  runtime objects. They come with our `P5AllocData` (the DLL fills it like the D3D10 driver does) and are treated as now.
- Monitored-fence storage is allocated by dxgkrnl itself in system memory (no `DxgkDdiCreateAllocation` call). guess.
- An allocation request without our magic is still refused, as now; the trace says so (phase 1 shows whether any comes).

## What 2.0 changes in Present / Render / Patch (the plan's known unknown)

- documented: physical (legacy patch mode) contexts keep `Render`/`Patch`/`SubmitCommand`; `Present` builds its DMA buffer
  for the context the present names. Our presents come from physical contexts (D3D10 driver, and the D3D12 driver's present
  context), so `DxgkDdiPresent` stays as it is. Flip presents still arrive as `SetVidPnSourceAddress`.
- guess: a present on a virtual context would come with `pDmaBuffer` but no patch list and be submitted with
  `SubmitCommandVirtual`; our `Present` already writes only one Op and the virtual submit runs Ops the same way, so it
  would work if it happens.

## Recovery and safety (unchanged rules)

- 2.0 is chosen by the volatile value `Session\FullVersion = 20` (plan: a restart always returns to 1.3), read once in
  DriverEntry; the registry value `FullVersion` is kept as the old switch.
- `StartPending` still refuses a start after a crashed one; the trace file is opened before the first adapter question.
- Every new entry point logs in and out (`Tr`), the first three calls of each in full after the first 1500 lines.

## Proven in the VM (phase 1, 2026-11-23): what 2.0 actually needed

KVM on CachyOS, the dev Windows (Kingston in the Sabrent enclosure, 19044, kernel 7725) from a RAM overlay, QEMU VGA
(vgamem_mb=256) as the "GPU", `ps5wddm-vm.sys` (`build-vm.sh`, `P5_VM`), Mesa's D3D10 driver on softpipe
(`build/gpusrc/mesa-build-sw`). Trace lines also go to QEMU's debug port (`isa-debugcon` 0x402) in the VM build, so a
hang or bug check still leaves the whole trace; bug checks were read from `KiBugCheckData` through the QEMU monitor and
symbolized with Microsoft's public PDBs (`symaddr.py`, `modlist.c`).

| # | Symptom | Cause | Fix (2.0 only) |
|---|---|---|---|
| 1 | Bug check 7E (C0000005, read of 0x40) in `dxgkrnl!DpiGetPnpRegistryKeyName`, called from `dxgmms2!VIDMM_GLOBAL::ReadPhysicalAdapterConfiguration` right after the segment questions | `PHYSICALADAPTERCAPS.DxgkPhysicalAdapterHandle` zeroed by our answer (Windows passes 0 in it) | answer dxgkrnl's `DeviceHandle` there. **Almost certainly the console's death of 2026-11-20** (same point: "right after the segment questions") |
| 2 | Bug check 113 (VIDEO_DXGKRNL_FATAL_ERROR) subtype 0x1C from `DMMVIDPNPRESENTPATH::SetRotationSupport`, under our `EnumVidPnCofuncModality` | 2.0 includes path-independent rotation; `RotationSupport` with only `Identity` set is refused | also set `Offset0` |
| 3 | DWM's flip re-issued at every vsync, second present blocks for ever | with `SetVidPnSourceAddressWithMultiPlaneOverlay2` registered, dxgkrnl sends flips there; our stub refused | do not register the two MPO2 entry points (no overlay planes) |
| 4 | same, after 3: flip queued as MMIO flip, never programmed | 2.0 programs MMIO flips at interrupt level (`VidSchiExecuteMmIoFlip...AtISR` via `DxgkCbSynchronizeExecution`); QEMU's VGA has no interrupt (`SynchronizeExecution` = C0000001) | **VM limitation**: the console's GPU has MSI (SynchronizeExecution works there since 1.3). `FlipOnVSyncMmIo=0` is not an escape: 2.0 then refuses the adapter |
| 5 | vsync interrupts switched off by `ControlInterrupt2` while a flip waits | — | report the vsync for a pending flip even with the interrupt off |
| 6 | `D3DKMTCreateContextVirtual` = C000000D | `DXGK_NODEMETADATA.GpuMmuSupported` (per engine) was 0 | settable (`Session\NodeGpuMmu=1`) but **open**: with it the system context becomes virtual and VidMm's init either bug-checks (7E, NULL in a GPU-VA mapping of an MDL, `dxgmms2+0xac15b` after `MapVirtualAddressRange`, paging buffers in system memory) or refuses the adapter (paging buffers in the segment). Phase-3 item, see below |
| VM | `DxgkCbSynchronizeExecution` fails (no interrupt) | QEMU VGA | VM build notifies at HIGH_LEVEL |

Seen working in 2.0: driver entry with the full 2.0 table, all adapter questions (13, 14 x4, 15, 16, segment v4, 10),
`CreateProcess` for system and programs, devices and contexts, **UPDATE_PAGE_TABLE** (15 updates, 3592 entries for the
paging process: level 0 is the leaf, entries for segment 1 carry segment offsets >> 12), FLUSH_TLB, paging transfers
and fills, monitored fences created by dxgkrnl for every process, DWM loading our D3D10 driver on its legacy context
(`pfnCreateContextCb` works on a 2.0 adapter, as documented), rendering, `PresentCb` accepted.

Open for phase 3 (virtual contexts, needed by the D3D12 fence path): give the adapter a second node, a copy/paging
engine with `GpuMmuSupported = FALSE` and `PagingNodeIndex` = that node, so the paging (system) context stays physical
while node 0 runs virtual contexts. That needs per-node fence ids and completions in the engine thread (2.0 only).

**Virtual contexts solved (console, 2026-11-23 ~11:20):** two engines (Session\NodeGpuMmu=1): node 0 physical 3D +
paging, node 1 3D with `GpuMmuSupported`; virtual contexts get `DmaBufferSegmentSet = 1` (0 = contiguous system memory
that VidMm cannot map: the 3B bug check). Walker maps table/fence pages through one-page MDLs. w2fence passes on the
console with DWM still on our D3D10 driver.
