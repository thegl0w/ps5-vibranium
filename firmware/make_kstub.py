#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Boot 22 kernel stub for ntoskrnl 10.0.19041 (boot.wim index 2, ntkrnlmp.pdb CA8E2F01B822EDE6357898BFBF8629971).

The PS5 freezes on the Windows logo with no visible stop code. This builds a small blob for the 0x7F7-byte slack at the
end of ntoskrnl's .text (same executable page) plus 5-byte jmp hooks at the start of early kernel/HAL functions:
  * breadcrumbs: each hooked function paints its own square in a row near the bottom of the screen, then continues;
  * KeBugCheck2: paints the stop code and its four parameters as hex digits at the top of the screen, then continues.
Drawing goes through the GPU's I/O-port window (MM_INDEX 0x2000 / MM_DATA 0x2004, bit 31 = VRAM offset), as the macOS
ktty/breadcrumbs do on this console: no page tables, no kernel services. The firmware fills the config words at
ExitBootServices time (mode, framebuffer VRAM offset, pitch, port) and installs the blob only after checking the image.
mode 0 = do nothing, 1 = GPU drawing (PS5), 2 = text on QEMU debugcon port 0x402 (VM tests only).
Usage: python3 make_kstub.py  (needs build/vm12/sym: ntoskrnl.exe and the public-symbol dumps; writes windows_kstub.inc)"""
import os, re, struct, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
KDIR = HERE.parent / os.environ.get("KSTUB_VMDIR", "build/vm12")   # build/vm7725 = ntoskrnl 10.0.19041.7725 (KB5127683)
SYM = KDIR / "sym"
LLVM = Path("/opt/homebrew/opt/llvm/bin")
sys.path.insert(0, str(KDIR))
import os
os.chdir(KDIR)                      # resolve.py reads sym/*.txt relative to here
from resolve import load

pe = (SYM / "ntoskrnl.exe").read_bytes()
e = struct.unpack_from("<I", pe, 0x3C)[0]
size_of_image = struct.unpack_from("<I", pe, e + 24 + 56)[0]
timestamp = struct.unpack_from("<I", pe, e + 8)[0]
secs = {}
for i in range(struct.unpack_from("<H", pe, e + 6)[0]):
    o = e + 24 + struct.unpack_from("<H", pe, e + 20)[0] + 40 * i
    name = pe[o:o + 8].rstrip(b"\0").decode()
    vs, va, rs, ro = struct.unpack_from("<IIII", pe, o + 8)
    secs[name] = (va, vs, ro, rs)
# Home: slack in the last page of a non-paged, non-discardable executable section (0x68000020 like .text).
# POOLCODE has 0xB70 bytes after its code; .text only 0x7F0. PAGE* sections are pageable and are not used.
HOME = "POOLCODE"
text_va, text_vs, text_ro, _ = secs[HOME]
STUB_RVA = (text_va + text_vs + 0xF) & ~0xF
STUB_MAX = text_va + ((text_vs + 0xFFF) & ~0xFFF) - STUB_RVA   # rest of that page
# Boot 15: a second code area in the slack after .text (0x7F0 bytes, lower RVA) for the page-fault recorder and its
# display, and 0x100 writable bytes at the end of .data's last page as its scratch memory.
EXT_HOME = ".text"
ext_va, ext_vs, _, _ = secs[EXT_HOME]
EXT_RVA = (ext_va + ext_vs + 0xF) & ~0xF
EXT_MAX = ext_va + ((ext_vs + 0xFFF) & ~0xFFF) - EXT_RVA
assert EXT_RVA < STUB_RVA
GAP = STUB_RVA - EXT_RVA                               # the two areas are assembled as one flat image
data_va, data_vs, _, data_rs = secs[".data"]
SCRATCH_LEN = 0x100
SCRATCH_RVA = data_va + ((data_vs + 0xFFF) & ~0xFFF) - SCRATCH_LEN
assert SCRATCH_RVA >= data_va + max(data_vs, data_rs)

def file_off(rva):
    for va, vs, ro, rs in secs.values():
        if va <= rva < va + max(vs, rs):
            return ro + rva - va
    raise KeyError(hex(rva))

syms = {n: a for a, n in load("nt")}

# Breadcrumb slots in expected boot order (slot 9 = halt, red). KeBugCheck2 also paints the stop code.
CRUMBS = ["KiSystemStartup", "KiInitializeBootStructures", "KdInitSystem", "KiInitializeKernel", "HalInitSystem",
          "HalpInitSystemPhase0", "HalpInterruptParseAcpiTables", "HalpApicInitializeIoUnit", "HalpInitializeInterrupts",
          "HalpTimerSelectRoles", "KeInitSystem", "MmInitSystem", "ObInitSystem", "SeInitSystem", "PsInitSystem",
          "PspInitPhase0", "Phase1Initialization", "Phase1InitializationDiscard", "HalpInitSystemPhase1",
          "KeStartAllProcessors", "InbvDriverInitialize", "BgkInitialize", "ExInitSystem", "CmInitSystem1",
          "IoInitSystem", "IopInitializeBootDrivers", "PnpBootDeviceWait", "IopMarkBootPartition", "IopCreateArcNames",
          "IopInitializeSystemDrivers", "PoInitSystem", "Phase1InitializationIoReady", "PspInitPhase1", "HaliHaltSystem"]
HALT_SLOT = CRUMBS.index("HaliHaltSystem")
HOOKS = [(n, i) for i, n in enumerate(CRUMBS) if n in syms] + [("KeBugCheck2", -1), ("KiNmiInterrupt", -2), ("KiPageFault", -3)]

def prologue(rva):
    """Whole instructions covering >= 5 bytes; refuse RIP-relative operands and control flow."""
    va = 0x140000000 + rva
    out = subprocess.run([LLVM / "llvm-objdump", "-d", f"--start-address={va:#x}", f"--stop-address={va + 24:#x}",
                          str(SYM / "ntoskrnl.exe")], capture_output=True, text=True, check=True).stdout
    n, riprel = 0, []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)", line)
        if not m:
            continue
        ins, ln = m.group(3), len(m.group(2).split())
        if re.match(r"(j|call|ret|loop|int)", ins):
            return None
        if "rip" in ins:
            if "$" in ins:          # an immediate follows the displacement: not handled
                return None
            riprel.append((n, ln))  # disp32 = the instruction's last 4 bytes
        n += ln
        if n >= 5:
            return pe[file_off(rva):file_off(rva) + n], riprel
    raise SystemExit(f"short prologue at {rva:#x}")

hooks = []
for name, slot in HOOKS:
    rva = syms[name]
    got = prologue(rva)
    if got is None:
        print(f"skipping {name}: prologue is RIP-relative or a branch")
        if slot < 0:
            raise SystemExit(f"{name} must be hookable")
        continue
    orig, riprel = got
    hooks.append((name, slot, rva, orig, riprel))

def k(rva):                       # a kernel RVA as a blob-relative expression (no relocations)
    return f"blob + ({rva:#x} - {STUB_RVA:#x})"

FONT = ["111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
        "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111",
        "010101111101101", "110101110101110", "111100100100111", "110101101101110", "111100111100111",
        "111100111100100"]

def hal_local_apic():
    """The HAL's local-APIC mapping (HalpLocalApic) for THIS kernel. Was a constant 0xC507F0, right only for 19041.1288
    (in 7725 that address is KseEngine+0x10; kernel pattern study, 2026-11-23). Taken from the public symbol and
    cross-checked against the code shape of the HAL's two accessors (build/kpattern/kfind.py): each must occur exactly
    once and both must name the same variable."""
    shapes = {"read": re.compile(rb"\x48\x8b\x05(....)\x48\x63\xd1\x8b\x04\x02\xc3", re.S),      # mov rax,[rip+d]; movsxd rdx,ecx; mov eax,[rdx+rax]; ret
              "write": re.compile(rb"\x48\x8b\x05(....)\x4c\x63\xc1\x41\x89\x14\x00\xc3", re.S)}  # mov rax,[rip+d]; movsxd r8,ecx; mov [r8+rax],edx; ret
    found = {}
    for kind, rx in shapes.items():
        hits = []
        for va, vs, ro, rs in secs.values():
            for m in rx.finditer(pe[ro:ro + min(vs, rs)]):
                hits.append(va + m.start() + 7 + struct.unpack("<i", m.group(1))[0])
        assert len(hits) == 1, f"local-APIC {kind} accessor: {len(hits)} hits"
        found[kind] = hits[0]
    assert found["read"] == found["write"], found
    v = found["read"]
    assert syms.get("HalpLocalApic", v) == v, (hex(syms["HalpLocalApic"]), hex(v))
    return v

HAL_APIC_VA_RVA = hal_local_apic()   # the HAL's local-APIC mapping (see HAL_APIC_VA below)
print(f"HalpLocalApic at rva {HAL_APIC_VA_RVA:#x} (symbol and accessor shapes agree)")
MEMSET_STORE = syms["memset"] + 0x78                   # memset's first store (movups [rcx], xmm0)
assert pe[file_off(MEMSET_STORE):file_off(MEMSET_STORE) + 3] == bytes.fromhex("0f1101")
PTE_BASE_VAR = syms["MmPteBase"]
def sc(o):                        # scratch qword o as a blob-relative expression
    return f"blob + ({SCRATCH_RVA + 8 * o:#x} - {STUB_RVA:#x})"
# Scratch qwords: 0 error code, 1 faulting RIP, 2 fault count, 3 memset return address, 4 memset destination, 5 memset length,
# 6 EFER bit 16 clears by pf_record, 7 sampler stopped (set when PspInitPhase1 starts; later crumbs no longer re-arm it).
ext = ["ext:",
       # pf_record: called first thing in KiPageFault. [rsp] = return, then the CPU frame: error code, RIP, CS, RFLAGS, RSP, SS.
       "pf_record:",
       "  push rax", "  push rcx",
       f"  lea rcx, [rip + {sc(0)}]",
       "  mov rax, qword ptr [rsp + 24]", "  mov qword ptr [rcx], rax",
       "  mov rax, qword ptr [rsp + 32]", "  mov qword ptr [rcx + 8], rax",
       "  inc qword ptr [rcx + 16]",
       f"  lea rax, [rip + blob + ({MEMSET_STORE:#x} - {STUB_RVA:#x})]", "  cmp rax, qword ptr [rcx + 8]", "  jne 1f",
       "  mov rax, qword ptr [rsp + 56]", "  mov rax, qword ptr [rax]", "  mov qword ptr [rcx + 24], rax",
       "  mov rax, qword ptr [rsp]", "  mov qword ptr [rcx + 32], rax",
       "  mov qword ptr [rcx + 40], r8",
       # A protection fault (P = 1) while EFER bit 16 is set: clear the bit, so a PTE with bit 58 is an ordinary page again.
       "1:", "  test byte ptr [rcx], 1", "  jz 3f", "  test byte ptr [rip + cfg_xofix], 1", "  jz 3f",
       "  push rdx", "  push rcx", "  mov ecx, 0xC0000080", "  rdmsr", "  btr eax, 16", "  pop rcx", "  jnc 4f",
       "  push rcx", "  mov ecx, 0xC0000080", "  wrmsr", "  pop rcx", "  inc qword ptr [rcx + 48]",
       "4:", "  pop rdx",
       "3:", "  pop rcx", "  pop rax", "  ret",
       # Boot 21, ap_fix: run by every processor when it enters KiSystemStartup (the other processors come in there too),
       # before it takes interrupts. As on macOS (ps5-hackintosh tools/make_ierfix.py), each processor needs: the six core
       # PerfCtl MSRs zeroed (Linux's watchdog counter is still armed on every processor) and AMD's extended-APIC IER0..7
       # opened (they come up with almost every vector masked; a processor left like that ignores IPIs).
       # GPU mode also paints a small yellow square per processor at x = 40 + 24 * APIC id, y = 1250.
       # All registers are saved by crumb_common.
       "ap_fix:",
       "  mov ecx, 0xC0010200", "  xor eax, eax", "  xor edx, edx", "  mov r9d, 6",
       "10:", "  wrmsr", "  add ecx, 2", "  dec r9d", "  jnz 10b",
       "  mov ecx, 0x1B", "  rdmsr", "  test eax, 0x400", "  jnz 11f",
       f"  mov rsi, qword ptr [rip + blob + ({HAL_APIC_VA_RVA:#x} - {STUB_RVA:#x})]", "  test rsi, rsi", "  jz 11f",
       "  mov eax, dword ptr [rsi + 0x30]", "  bt eax, 31", "  jnc 11f",
       "  mov eax, dword ptr [rsi + 0x400]", "  bt eax, 0", "  jnc 11f",
       "  mov eax, dword ptr [rsi + 0x410]", "  bt eax, 0", "  jnc 11f"] + \
      [f"  mov dword ptr [rsi + {0x480 + 16 * n:#x}], 0xFFFFFFFF" for n in range(8)] + \
      ["11:", "  cmp dword ptr [rip + cfg_mode], 1", "  jne 12f",
       "  mov eax, 1", "  cpuid", "  shr ebx, 24",
       "  imul edi, ebx, 24", "  add edi, 40", "  mov esi, 1250", "  mov edx, 18", "  mov ecx, 30", "  mov r8d, 0x00FFFF00",
       "  call fill_rect",
       "12:", "  ret",
       # rva_or_raw: rax = address -> its RVA when inside ntoskrnl, else unchanged. Clobbers rcx, rdx.
       "rva_or_raw:",
       "  lea rcx, [rip + blob]", f"  sub rcx, {STUB_RVA:#x}",
       "  mov rdx, rax", "  sub rdx, rcx", f"  cmp rdx, {size_of_image:#x}", "  jae 2f", "  mov rax, rdx", "2:", "  ret",
       # pte_addr: rax = virtual address -> rax = address of the entry that maps it (r13 = PTE base).
       "pte_addr:",
       "  shr rax, 9", "  mov rcx, 0x7FFFFFFFF8", "  and rax, rcx", "  add rax, r13", "  ret",
       # fault_block: 12 rows at x = 60, y = 500 + 44*row (8 px cells, pale yellow). Called from the NMI sampler.
       # Rows: 0 fault count (low 32) : error code, 1 CR2, 2 faulting RIP, 3 memset's caller, 4 memset destination,
       # 5 memset length, 6 CR3, 7 PML4E, 8 PDPTE, 9 PDE, 10 PTE for CR2 (walk stops at a non-present or large entry),
       # 11 CR4 : EFER, 12 number of times pf_record had to clear EFER bit 16. Debugcon mode prints them as 'F' lines.
       "fault_block:",
       "  push rbx", "  push r12", "  push r13", "  push r14", "  push rbp", "  sub rsp, 104",
       f"  lea rsi, [rip + {sc(0)}]",
       "  mov eax, dword ptr [rsi + 16]", "  shl rax, 32", "  mov ecx, dword ptr [rsi]", "  or rax, rcx", "  mov qword ptr [rsp], rax",
       "  mov rax, cr2", "  mov qword ptr [rsp + 8], rax", "  mov r12, rax",
       "  mov rax, qword ptr [rsi + 8]", "  call rva_or_raw", "  mov qword ptr [rsp + 16], rax",
       "  mov rax, qword ptr [rsi + 24]", "  call rva_or_raw", "  mov qword ptr [rsp + 24], rax",
       "  mov rax, qword ptr [rsi + 32]", "  mov qword ptr [rsp + 32], rax",
       "  mov rax, qword ptr [rsi + 40]", "  mov qword ptr [rsp + 40], rax",
       "  mov rax, cr3", "  mov qword ptr [rsp + 48], rax",
       "  xor eax, eax", "  mov qword ptr [rsp + 56], rax", "  mov qword ptr [rsp + 64], rax", "  mov qword ptr [rsp + 72], rax", "  mov qword ptr [rsp + 80], rax",
       "  mov rax, cr4", "  shl rax, 32", "  mov r14, rax", "  mov ecx, 0xC0000080", "  rdmsr", "  mov eax, eax", "  or rax, r14", "  mov qword ptr [rsp + 88], rax",
       "  mov rax, qword ptr [rsi + 48]", "  mov qword ptr [rsp + 96], rax",
       f"  mov r13, qword ptr [rip + blob + ({PTE_BASE_VAR:#x} - {STUB_RVA:#x})]", "  test r13, r13", "  jz 5f",
       "  mov rax, r12", "  call pte_addr", "  mov r12, rax",          # r12 = PTE address
       "  call pte_addr", "  mov r14, rax",                            # r14 = PDE address
       "  call pte_addr", "  mov rbx, rax",                            # rbx = PDPTE address
       "  call pte_addr",                                              # rax = PML4E address
       "  mov rax, qword ptr [rax]", "  mov qword ptr [rsp + 56], rax", "  test al, 1", "  jz 5f",
       "  mov rax, qword ptr [rbx]", "  mov qword ptr [rsp + 64], rax", "  test al, 1", "  jz 5f", "  test al, 0x80", "  jnz 5f",
       "  mov rax, qword ptr [r14]", "  mov qword ptr [rsp + 72], rax", "  test al, 1", "  jz 5f", "  test al, 0x80", "  jnz 5f",
       "  mov rax, qword ptr [r12]", "  mov qword ptr [rsp + 80], rax",
       "5:", "  mov r12, rsp", "  xor ebx, ebx",
       "  mov eax, dword ptr [rip + cfg_mode]", "  cmp eax, 2", "  je 7f", "  cmp eax, 1", "  jne 9f",
       "  mov ebp, 8",
       "6:", "  mov rax, qword ptr [r12 + rbx*8]", "  mov edi, 60", "  imul esi, ebx, 44", "  add esi, 500", "  mov r8d, 0x00FFFF80",
       "  call hex64", "  inc ebx", "  cmp ebx, 13", "  jb 6b", "  jmp 9f",
       "7:", "  mov al, 0x46", "  call dbg_char", "  mov eax, ebx", "  call dbg_hexnib", "  mov al, 0x20", "  call dbg_char",
       "  mov r13, qword ptr [r12 + rbx*8]", "  mov r14d, 16",
       "8:", "  rol r13, 4", "  mov eax, r13d", "  call dbg_hexnib", "  dec r14d", "  jnz 8b",
       "  mov al, 10", "  call dbg_char", "  inc ebx", "  cmp ebx, 13", "  jb 7b",
       "9:", "  add rsp, 104", "  pop rbp", "  pop r14", "  pop r13", "  pop r12", "  pop rbx", "  ret",
       "ext_end:",
       f"  .org {GAP:#x}"]
asm = [".intel_syntax noprefix", ".text"] + ext + ["blob:",
       "  .ascii \"K22S\"",
       "cfg_mode:  .long 0", "cfg_fboff: .long 0", "cfg_pitch: .long 0", "cfg_port:  .long 0x2000",
       "cfg_sampler: .long 1", "cfg_period: .long 0x80000000",   # NMI sampler on; PERF_CTR0 = 0xFFFF:period (2^31 cycles)
       # cfg_xofix: 1 = keep EFER bit 16 (Sony's xotext enable) clear (set when the firmware's own write worked).
       # VM experiments: 2 = set the bit when ExInitSystem starts and leave it (reproduce the PS5 loop);
       # 3 = set it there once and let pf_record clear it on the first protection fault (recovery).
       "cfg_xofix: .long 0",
       "font: " + "\n".join(f"  .short {int(b, 2):#x}" for b in FONT),
       # fill_rect: edi=x esi=y edx=w ecx=h r8d=colour (GPU mode). Clobbers rax, r9-r11.
       "hi0:", "  push rax", "  push rdx", "  mov edx, dword ptr [rip + cfg_port]", "  add edx, 0x18", "  xor eax, eax",
       "  out dx, eax", "  pop rdx", "  pop rax", "  ret",
       "fill_rect:",
       "  push rbx", "  push rdx", "  push rcx", "  call hi0",
       "  mov ebx, dword ptr [rip + cfg_pitch]",
       "  mov r9d, ecx",                                   # rows left
       "  mov eax, esi", "  imul eax, ebx", "  add eax, dword ptr [rip + cfg_fboff]",
       "  lea eax, [rax + rdi*4]", "  mov r11d, eax",        # row start offset
       "1:", "  test r9d, r9d", "  jz 3f",
       "  mov r10d, dword ptr [rsp + 8]", "  mov eax, r11d",  # cols left (saved edx)
       "2:", "  test r10d, r10d", "  jz 4f",
       "  push rax", "  or eax, 0x80000000", "  mov edx, dword ptr [rip + cfg_port]", "  out dx, eax",
       "  add edx, 4", "  mov eax, r8d", "  out dx, eax", "  pop rax",
       "  add eax, 4", "  dec r10d", "  jmp 2b",
       "4:", "  add r11d, ebx", "  dec r9d", "  jmp 1b",
       "3:", "  pop rcx", "  pop rdx", "  pop rbx", "  ret",
       # dbg_char: al -> debugcon
       "dbg_char:", "  push rdx", "  mov dx, 0x402", "  out dx, al", "  pop rdx", "  ret",
       "dbg_hexnib:", "  and al, 0xF", "  cmp al, 10", "  jb 5f", "  add al, 7", "5:", "  add al, 0x30", "  jmp dbg_char",
       # crumb: edi = slot (or 9 = halt). Paints a 56x36 square at x = 40 + 72*slot, y = 1340.
       "crumb:",
       "  mov eax, dword ptr [rip + cfg_mode]",
       "  cmp eax, 2", "  je 6f", "  cmp eax, 1", "  jne 7f",
       "  push rdi", "  push rsi", "  push rdx", "  push rcx", "  push r8",
       "  mov r8d, 0x0000FF00", "  test edi, 1", "  jz 8f", "  mov r8d, 0x0000FFFF", "8:",
       "  mov eax, edi", "  and eax, 3", "  cmp eax, 3", "  jne 27f", "  mov r8d, 0x00FFFFFF", "27:",   # every 4th white: a ruler
       f"  cmp edi, {HALT_SLOT}", "  jne 9f", "  mov r8d, 0x00FF0000", "9:",
       "  imul edi, edi, 72", "  add edi, 40", "  mov esi, 1340", "  mov edx, 56", "  mov ecx, 36", "  call fill_rect",
       "  pop r8", "  pop rcx", "  pop rdx", "  pop rsi", "  pop rdi", "7:", "  ret",
       "6:", "  mov al, 0x43", "  call dbg_char", "  mov eax, edi", "  shr eax, 4", "  call dbg_hexnib", "  mov eax, edi", "  call dbg_hexnib", "  mov al, 10", "  jmp dbg_char",
       # hex64: rax = value, edi = x, esi = y, r8d = colour. 16 digits, 3x5 cells of 10 px, 40 px apart.
       "hex64:",
       "  push rbx", "  push r12", "  push r13", "  push r14", "  push r15", "  push rdx", "  push rcx",
       "  mov r12, rax", "  mov r13d, 16",
       "10:", "  rol r12, 4", "  mov eax, r12d", "  and eax, 0xF",
       "  lea rbx, [rip + font]", "  movzx r14d, word ptr [rbx + rax*2]",
       "  mov r15d, 14",                                   # bit index, row-major from top-left
       "11:", "  bt r14d, r15d", "  push r8", "  jc 12f", "  mov r8d, 0x00202020", "12:",
       "  push rdi", "  push rsi",
       "  mov eax, 14", "  sub eax, r15d", "  xor edx, edx", "  mov ecx, 3", "  div ecx",   # eax=row edx=col
       "  imul eax, ebp", "  add esi, eax", "  imul edx, ebp", "  add edi, edx",
       "  mov edx, ebp", "  mov ecx, ebp", "  call fill_rect",
       "  pop rsi", "  pop rdi", "  pop r8",
       "  dec r15d", "  jns 11b",
       "  lea edi, [rdi + rbp*4]", "  dec r13d", "  jnz 10b",
       "  pop rcx", "  pop rdx", "  pop r15", "  pop r14", "  pop r13", "  pop r12", "  pop rbx", "  ret",
       # bugcheck: rbx -> five qwords (code, p1..p4)
       "bugcheck:",
       "  mov eax, dword ptr [rip + cfg_mode]", "  cmp eax, 2", "  je 20f", "  cmp eax, 1", "  jne 21f",
       # Nested bugchecks (KeBugCheck2 re-enters with 0x1E from its debug break) must not paint over the first one:
       # pixel (0,0) holds 0xA5BC5C00 | count, read back through the window. Blocks of 5 rows, 336 px apart, max 3.
       "  call hi0",
       "  mov edx, dword ptr [rip + cfg_port]", "  mov eax, dword ptr [rip + cfg_fboff]", "  or eax, 0x80000000",
       "  out dx, eax", "  add edx, 4", "  in eax, dx",
       "  mov ecx, eax", "  shr ecx, 8", "  xor r13d, r13d", "  cmp ecx, 0xA5BC5C", "  jne 26f", "  movzx r13d, al", "26:",
       "  cmp r13d, 3", "  jae 21f",
       "  lea eax, [r13 + 1]", "  or eax, 0xA5BC5C00", "  out dx, eax",       # MM_INDEX still points at pixel (0,0)
       "  imul r13d, r13d, 336", "  add r13d, 120",
       "  xor r12d, r12d",
       "22:", "  mov rax, qword ptr [rbx + r12*8]", "  mov edi, 60", "  mov esi, r12d", "  imul esi, esi, 64", "  add esi, r13d",
       "  mov r8d, 0x00FFFFFF", "  test r12d, r12d", "  jnz 23f", "  mov r8d, 0x00FFFF00", "23:",
       "  call hex64", "  inc r12d", "  cmp r12d, 5", "  jb 22b", "21:", "  ret",
       "20:", "  xor r12d, r12d",
       "24:", "  mov al, 0x42", "  call dbg_char", "  mov r13, qword ptr [rbx + r12*8]", "  mov r14d, 16",
       "25:", "  rol r13, 4", "  mov eax, r13d", "  call dbg_hexnib", "  dec r14d", "  jnz 25b",
       "  mov al, 10", "  call dbg_char", "  inc r12d", "  cmp r12d, 5", "  jb 24b", "  ret"]

HAL_APIC_VA = HAL_APIC_VA_RVA   # HalpApic1ReadRegister: mov rax, [rip -> this]; mov eax, [rax + reg] (per build)
APIC_SLOTS = {9: 0, 10: 1, 16: 2}   # HalpTimerSelectRoles, KeInitSystem, Phase1Initialization -> state block 0..2
def rd(reg, msr):
    return [f"  mov eax, dword ptr [rsi + {reg:#x}]"] if msr is None else [f"  mov ecx, {msr:#x}", "  rdmsr"]
asm += ["apicstate:",                               # edi = block; 8 qwords at [rsp]
        "  push rbp", "  push rbx", "  push r12", "  sub rsp, 64",
        "  mov ecx, 0x1B", "  rdmsr", "  shl rdx, 32", "  or rax, rdx", "  mov qword ptr [rsp], rax",
        "  test eax, 0x400", "  jnz 40f",
        f"  mov rsi, qword ptr [rip + {k(HAL_APIC_VA)}]", "  mov qword ptr [rsp + 8], rsi",
        "  xor eax, eax", "  mov qword ptr [rsp + 16], rax", "  mov qword ptr [rsp + 24], rax", "  mov qword ptr [rsp + 32], rax",
        "  mov qword ptr [rsp + 40], rax", "  mov qword ptr [rsp + 48], rax", "  mov qword ptr [rsp + 56], rax",
        "  test rsi, rsi", "  jz 45f"]
def pair(slot, hi, lo, msr_hi=None, msr_lo=None, x2=False):
    out = []
    out += ([f"  mov ecx, {msr_hi:#x}", "  rdmsr"] if x2 else [f"  mov eax, dword ptr [rsi + {hi:#x}]"])
    out += ["  mov r12d, eax", "  shl r12, 32"]
    if lo is not None or msr_lo is not None:
        out += ([f"  mov ecx, {msr_lo:#x}", "  rdmsr"] if x2 else [f"  mov eax, dword ptr [rsi + {lo:#x}]"])
        out += ["  or r12, rax"]
    out += [f"  mov qword ptr [rsp + {slot * 8}], r12"]
    return out
asm += pair(2, 0x30, 0x400) + pair(3, 0x410, 0x4E0)
asm += ["  mov eax, dword ptr [rsi + 0x30]", "  bt eax, 31", "  jnc 41f",          # re-open AMD IER from inside the kernel
        "  mov eax, dword ptr [rsi + 0x400]", "  bt eax, 0", "  jnc 41f",
        "  mov eax, dword ptr [rsi + 0x410]", "  bt eax, 0", "  jnc 41f"] + \
       [f"  mov dword ptr [rsi + {0x480 + 16 * n:#x}], 0xFFFFFFFF" for n in range(8)] + ["41:"]
asm += pair(4, 0xF0, 0x80) + pair(5, 0x260, 0x160) + pair(6, 0x270, 0x170) + pair(7, 0x320, 0x390)
asm += ["  jmp 45f", "40:", "  mov qword ptr [rsp + 8], 2", "  mov qword ptr [rsp + 24], 0"]
asm += pair(2, None, None, 0x803, None, True)[:-1] + ["  mov qword ptr [rsp + 16], r12"]
asm += pair(4, None, None, 0x80F, 0x808, True) + pair(5, None, None, 0x826, 0x816, True) + \
       pair(6, None, None, 0x827, 0x817, True) + pair(7, None, None, 0x832, 0x839, True)
asm += ["45:", "  mov rbx, rsp",
        "  mov eax, dword ptr [rip + cfg_mode]", "  cmp eax, 2", "  je 46f", "  cmp eax, 1", "  jne 49f",
        "  mov ebp, 8", "  mov r8d, 0x00C0FFC0", "  mov r12d, 0", "  mov r11d, edi",
        "47:", "  mov rax, qword ptr [rbx + r12*8]",
        "  mov edi, r11d", "  imul edi, edi, 540", "  add edi, 740",
        "  mov esi, r12d", "  imul esi, esi, 52", "  add esi, 120",
        "  cmp r11d, 3", "  jne 51f", "  mov edi, 1820", "  add esi, 580", "51:",
        "  push r11", "  call hex64", "  pop r11",
        "  inc r12d", "  cmp r12d, 8", "  jb 47b", "  jmp 49f",
        "46:", "  mov al, 0x41", "  call dbg_char", "  mov eax, edi", "  call dbg_hexnib", "  xor r12d, r12d",
        "48:", "  mov al, 0x20", "  call dbg_char", "  push r13", "  push r14", "  mov r13, qword ptr [rbx + r12*8]", "  mov r14d, 16",
        "50:", "  rol r13, 4", "  mov eax, r13d", "  call dbg_hexnib", "  dec r14d", "  jnz 50b", "  pop r14", "  pop r13",
        "  inc r12d", "  cmp r12d, 8", "  jb 48b", "  mov al, 10", "  call dbg_char",
        "49:", "  add rsp, 64", "  pop r12", "  pop rbx", "  pop rbp", "  ret"]

# Trampolines. Save everything the routines touch, run them, restore, replay the displaced bytes, jump back.
SAVE = ["rax", "rcx", "rdx", "rbx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
# Breadcrumb trampolines are 'push slot; call crumb_common; <displaced>; jmp back' (crumb_common drops the slot: ret 8).
# NMI sampler: PERF_CTR0 counts unhalted cycles (event 0x76, the same PerfCtl value as Linux's NMI watchdog) and its
# overflow is delivered as an NMI through LVTPC. Every breadcrumb from slot 3 on (kernel IDT loaded) restarts the count,
# so samples appear only when a phase runs for more than one period without reaching the next crumb. The hook on
# KiNmiInterrupt handles its own NMIs completely (draw, re-arm, iretq); any other NMI goes on to Windows.
# Sample rows: x = 720, y = 900 + 48*(n & 7), 8 px cells; the value is RIP as an RVA when inside ntoskrnl, else the raw
# RIP; white = interrupts were enabled, orange = disabled. A white bar at x = 690 marks the newest row. The sample
# number lives in pixel (1,0) (0xA55A0000 | n). Every 4th sample redraws APIC state block 3.
asm += ["rearm:",
        "  cmp dword ptr [rip + cfg_sampler], 0", "  je 71f",
        "  push rax", "  push rcx", "  push rdx",
        "  mov ecx, 0xC0010000", "  xor eax, eax", "  xor edx, edx", "  wrmsr",
        "  mov ecx, 0xC0010004", "  mov eax, dword ptr [rip + cfg_period]", "  mov edx, 0xFFFF", "  wrmsr",
        "  mov ecx, 0x1B", "  rdmsr", "  test eax, 0x400", "  jnz 72f",
        f"  mov rax, qword ptr [rip + {k(HAL_APIC_VA)}]", "  test rax, rax", "  jz 73f",
        "  mov dword ptr [rax + 0x340], 0x400", "  jmp 73f",
        "72:", "  mov ecx, 0x834", "  mov eax, 0x400", "  xor edx, edx", "  wrmsr",
        "73:", "  mov ecx, 0xC0010000", "  mov eax, 0x530076", "  xor edx, edx", "  wrmsr",
        "  pop rdx", "  pop rcx", "  pop rax",
        "71:", "  ret",
        # nmi_draw: rax = value, r8d = colour (GPU mode). Returns the sample number in eax.
        "nmi_draw:",
        "  push rbx", "  push r12", "  push r13", "  push rbp",
        "  mov r12, rax", "  call hi0",
        "  mov edx, dword ptr [rip + cfg_port]", "  mov eax, dword ptr [rip + cfg_fboff]", "  add eax, 4", "  or eax, 0x80000000",
        "  out dx, eax", "  add edx, 4", "  in eax, dx",
        "  mov ecx, eax", "  shr ecx, 16", "  xor r13d, r13d", "  cmp ecx, 0xA55A", "  jne 74f", "  movzx r13d, ax", "74:",
        "  lea eax, [r13 + 1]", "  and eax, 0xFFFF", "  or eax, 0xA55A0000", "  out dx, eax",   # MM_INDEX still at pixel (1,0)
        "  mov ebp, 8", "  mov rax, r12", "  mov edi, 720",
        "  mov esi, r13d", "  and esi, 7", "  imul esi, esi, 48", "  add esi, 900",
        "  call hex64",
        "  mov edi, 690", "  mov edx, 20", "  mov ecx, 40", "  mov r8d, 0x00FFFFFF", "  call fill_rect",
        "  lea esi, [r13 + 7]", "  and esi, 7", "  imul esi, esi, 48", "  add esi, 900",
        "  mov edi, 690", "  mov edx, 20", "  mov ecx, 40", "  xor r8d, r8d", "  call fill_rect",
        "  mov eax, r13d",
        "  pop rbp", "  pop r13", "  pop r12", "  pop rbx", "  ret",
        # nmi_sample: rsi -> the NMI's iret frame (RIP, CS, RFLAGS, RSP, SS). All registers are saved by the caller.
        "nmi_sample:",
        "  mov rbx, rsi",
        "  mov r12, qword ptr [rbx]",
        "  lea rcx, [rip + blob]", f"  sub rcx, {STUB_RVA:#x}",
        "  mov rdx, r12", "  sub rdx, rcx", f"  cmp rdx, {size_of_image:#x}", "  jae 75f", "  mov r12, rdx", "75:",
        "  mov eax, dword ptr [rip + cfg_mode]", "  cmp eax, 2", "  je 77f", "  cmp eax, 1", "  jne 79f",
        "  mov edx, dword ptr [rip + cfg_port]", "  in eax, dx", "  mov r14d, eax",       # MM_INDEX of the interrupted code
        "  mov rax, r12", "  mov r8d, 0x00FFFFFF",
        "  test dword ptr [rbx + 16], 0x200", "  jnz 76f", "  mov r8d, 0x00FFA000", "76:",
        "  call nmi_draw",
        "  test eax, 3", "  jnz 78f", "  mov edi, 3", "  call apicstate", "78:", "  call fault_block",
        "  mov edx, dword ptr [rip + cfg_port]", "  mov eax, r14d", "  out dx, eax",
        "  jmp 79f",
        "77:", "  mov al, 0x4E", "  call dbg_char", "  mov r13, r12", "  mov r14d, 16",
        "80:", "  rol r13, 4", "  mov eax, r13d", "  call dbg_hexnib", "  dec r14d", "  jnz 80b",
        "  mov al, 0x69", "  test dword ptr [rbx + 16], 0x200", "  jz 81f", "  mov al, 0x49", "81:", "  call dbg_char",
        "  mov al, 10", "  call dbg_char", "  call fault_block",
        "79:", "  call rearm", "  ret"]
# Boot 17: the sampler stops when PspInitPhase1 starts (Windows is about to run user mode), so nothing is drawn over the desktop.
asm += ["crumb_common:", "  pushfq"] + [f"  push {r}" for r in SAVE] + [f"  mov edi, dword ptr [rsp + {8 * (len(SAVE) + 2)}]", "  mov r15d, edi", "  call crumb",
        "  test r15d, r15d", "  jnz 64f", "  call ap_fix", "64:"] + \
       sum([[f"  cmp r15d, {sl}", f"  jne 6{b}f", f"  mov edi, {b}", "  call apicstate", f"6{b}:"] for sl, b in APIC_SLOTS.items()], []) + \
       ["  cmp r15d, 3", "  jb 69f", f"  cmp byte ptr [rip + {sc(7)}], 0", "  jne 69f",      # scratch 7: sampler stopped for good
        f"  cmp r15d, {CRUMBS.index('PspInitPhase1')}", "  je 65f", "  call rearm", "  jmp 69f",
        "65:", f"  mov byte ptr [rip + {sc(7)}], 1", "  mov ecx, 0xC0010000", "  xor eax, eax", "  xor edx, edx", "  wrmsr", "69:",
        "  mov ebx, dword ptr [rip + cfg_xofix]", "  test ebx, ebx", "  jz 68f", "  mov ecx, 0xC0000080", "  rdmsr",
        "  cmp ebx, 1", "  jne 67f", "  btr eax, 16", "  jnc 68f", "  jmp 66f",
        f"67:", f"  cmp r15d, {CRUMBS.index('ExInitSystem')}", "  jne 68f", "  bts eax, 16",
        "66:", "  wrmsr", "68:"] + \
       [f"  pop {r}" for r in reversed(SAVE)] + ["  popfq", "  ret 8"]
NMI_SAVE = ["rbx", "rsi", "rdi", "rbp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
for name, slot, rva, orig, riprel in hooks:
    asm += [f"tr_{name}:"]
    if slot == -2:    # KiNmiInterrupt: [rsp] = the iret frame. Ours = our PerfCtl value and PERF_CTR0 past overflow (bit 47 clear).
        asm += ["  push rax", "  push rcx", "  push rdx",
                "  cmp dword ptr [rip + cfg_mode], 0", "  je 90f", "  cmp dword ptr [rip + cfg_sampler], 0", "  je 90f",
                "  mov ecx, 0xC0010000", "  rdmsr", "  cmp eax, 0x530076", "  jne 90f",
                "  mov ecx, 0xC0010004", "  rdmsr", "  bt edx, 15", "  jc 90f"] + [f"  push {r}" for r in NMI_SAVE] + [
                f"  lea rsi, [rsp + {8 * (3 + len(NMI_SAVE))}]", "  call nmi_sample"] + [f"  pop {r}" for r in reversed(NMI_SAVE)] + [
                "  pop rdx", "  pop rcx", "  pop rax", "  iretq",
                "90:", "  pop rdx", "  pop rcx", "  pop rax"]
    elif slot == -3:  # KiPageFault: record the fault (error code, RIP, count) in scratch memory; hot path, kept tiny
        asm += ["  call pf_record"]
    elif slot < 0:    # KeBugCheck2: entry rsp+0x28 = 5th argument; we push 15 qwords (0x78)
        asm += ["  pushfq"] + [f"  push {r}" for r in SAVE] + [
                "  mov rax, qword ptr [rsp + 0x78 + 0x28]", "  push rax", "  push r9", "  push r8", "  push rdx", "  push rcx",
                "  mov rbx, rsp", "  push rbp", "  mov ebp, 10", "  call bugcheck", "  pop rbp", "  add rsp, 40"] + [f"  pop {r}" for r in reversed(SAVE)] + ["  popfq"]
    else:
        asm += [f"  push {slot}", "  call crumb_common"]
    # 2026-11-22: no kernel bytes in our output. The slot is filled at boot by the firmware with the instructions the
    # hook displaces, copied from the loaded kernel itself (and RIP-relative displacements adjusted there).
    asm += [f"disp_{name}:", "  .byte " + ",".join(["0xcc"] * len(orig)), f"  jmp {k(rva + len(orig))}"]
asm += ["blob_end:"]

with tempfile.TemporaryDirectory() as t:
    src, obj, binf = Path(t, "k.s"), Path(t, "k.o"), Path(t, "k.bin")
    src.write_text("\n".join(asm) + "\n")
    subprocess.run([LLVM / "llvm-mc", "-triple=x86_64-pc-linux-gnu", "-filetype=obj", "-o", obj, src], check=True)
    sym = subprocess.run([LLVM / "llvm-nm", obj], capture_output=True, text=True, check=True).stdout
    rel = subprocess.run([LLVM / "llvm-objdump", "-r", obj], capture_output=True, text=True).stdout
    if "R_X86_64" in rel:
        raise SystemExit("unexpected relocations:\n" + rel)
    subprocess.run([LLVM / "llvm-objcopy", "-O", "binary", "--only-section=.text", obj, binf], check=True)
    flat = binf.read_bytes()
    offs = {l.split()[2]: int(l.split()[0], 16) for l in sym.splitlines() if len(l.split()) == 3}
assert offs["blob"] == GAP and offs["ext"] == 0
ext_blob = flat[:offs["ext_end"]]
assert flat[len(ext_blob):GAP] == bytes(GAP - len(ext_blob))
if len(ext_blob) > EXT_MAX:
    raise SystemExit(f"ext {len(ext_blob)} > slack {EXT_MAX}")
ext_offs = {n: o for n, o in offs.items() if o < GAP}
offs = {n: o - GAP for n, o in offs.items() if o >= GAP}
blob = bytes(flat[GAP:])
for name, slot, rva, orig, riprel in hooks:
    assert blob[offs["disp_" + name]:offs["disp_" + name] + len(orig)] == b"\xcc" * len(orig)
    assert len(riprel) <= 2, name
    for o, ln in riprel:
        print(f"  rip-relative instruction in {name}: offset {o}, length {ln} (adjusted by the firmware at boot)")


def fnv64(b):
    """FNV-1a, 64 bit: the firmware compares the kernel's bytes against these fingerprints (no kernel bytes stored)."""
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h
if len(blob) > STUB_MAX:
    raise SystemExit(f"blob {len(blob)} > slack {STUB_MAX}")
raw_end = text_va + secs[HOME][3]                     # past the raw data the loader zero-fills
slack = pe[file_off(STUB_RVA):file_off(STUB_RVA) + max(0, min(STUB_RVA + len(blob), raw_end) - STUB_RVA)]
assert slack == bytes(len(slack)), "slack not zero in the file"   # the firmware re-checks it in memory

h = ["// SPDX-License-Identifier: MIT",
     f"// Generated by make_kstub.py. ntoskrnl SizeOfImage {size_of_image:#x}, TimeDateStamp {timestamp:#x}.",
     "// Holds only our code, offsets and FNV-1a fingerprints: no bytes of the kernel (the firmware copies those at boot).",
     f"#define KSTUB_IMAGE_SIZE {size_of_image:#x}U", f"#define KSTUB_TIMESTAMP {timestamp:#x}U",
     f"#define KSTUB_RVA {STUB_RVA:#x}U", f"#define KSTUB_SLACK {STUB_MAX:#x}U",
     f"#define KSTUB_CFG_MODE {offs['cfg_mode']:#x}U", f"#define KSTUB_CFG_FBOFF {offs['cfg_fboff']:#x}U",
     f"#define KSTUB_CFG_PITCH {offs['cfg_pitch']:#x}U", f"#define KSTUB_CFG_PORT {offs['cfg_port']:#x}U",
     f"#define KSTUB_CFG_SAMPLER {offs['cfg_sampler']:#x}U", f"#define KSTUB_CFG_PERIOD {offs['cfg_period']:#x}U",
     f"#define KSTUB_CFG_XOFIX {offs['cfg_xofix']:#x}U",
     f"#define KSTUB_OFF_CRUMB {offs['crumb']:#x}U", f"#define KSTUB_OFF_BUGCHECK {offs['bugcheck']:#x}U",
     f"#define KSTUB_OFF_NMIDRAW {offs['nmi_draw']:#x}U",
     f"#define KSTUB_EXT_RVA {EXT_RVA:#x}U", f"#define KSTUB_EXT_SLACK {EXT_MAX:#x}U",
     f"#define KSTUB_SCRATCH_RVA {SCRATCH_RVA:#x}U", f"#define KSTUB_SCRATCH_LEN {SCRATCH_LEN:#x}U",
     f"#define KSTUB_EXT_OFF_FAULTBLOCK {ext_offs['fault_block']:#x}U", f"#define KSTUB_EXT_OFF_PFRECORD {ext_offs['pf_record']:#x}U",
     "STATIC CONST UINT8 mKstubExt[] = {" + ",".join(f"0x{b:02x}" for b in ext_blob) + "};",
     f"STATIC CONST UINT64 mKstubPre[1] = {{ 0x{fnv64(pe[file_off(STUB_RVA - 16):file_off(STUB_RVA)]):016x}ULL }};   // FNV-1a of the 16 bytes before the slack",
     f"#define KSTUB_PROBE_RVA {syms['HalpApicInitializeIoUnit'] + 0x5D:#x}U   // the HAL probe block patched by windows_halpatch.inc",
     "STATIC CONST UINT8 mKstubBlob[] = {" + ",".join(f"0x{b:02x}" for b in blob) + "};",
     "typedef struct { UINT32 Rva; UINT32 Tramp; UINT32 Disp; UINT8 Len; UINT8 RipCount; UINT8 RipOff[2]; UINT8 RipLen[2]; UINT64 Hash; } KSTUB_HOOK;",
     "STATIC CONST KSTUB_HOOK mKstubHooks[] = {"]
for name, slot, rva, orig, riprel in hooks:
    ro = [o for o, ln in riprel] + [0] * (2 - len(riprel))
    rl = [ln for o, ln in riprel] + [0] * (2 - len(riprel))
    h.append(f"  {{ {rva:#x}, {offs['tr_' + name]:#x}, {offs['disp_' + name]:#x}, {len(orig)}, {len(riprel)}, "
             f"{{ {ro[0]}, {ro[1]} }}, {{ {rl[0]}, {rl[1]} }}, 0x{fnv64(orig):016x}ULL }},  // {name} slot {slot}")
h.append("};")
(Path(os.environ["KSTUB_OUT"]) if os.environ.get("KSTUB_OUT") else HERE / "windows_kstub.inc").write_text("\n".join(h) + "\n")
test = HERE.parent / "build/kstub-test"                    # for the user-mode drawing harness
(test / "blob.bin").write_bytes(blob)
(test / "ext.bin").write_bytes(ext_blob)
(test / "offs.h").write_text("".join(re.sub(r"(0x[0-9a-f]+)U", r"\1", l) + "\n" for l in h if l.startswith("#define")))
ext_raw_end = ext_va + secs[EXT_HOME][3]
ext_file = pe[file_off(EXT_RVA):file_off(EXT_RVA) + max(0, min(EXT_RVA + EXT_MAX, ext_raw_end) - EXT_RVA)]
assert ext_file == bytes(len(ext_file)), ".text slack not zero in the file"
print(f"ext {len(ext_blob)} bytes of {EXT_MAX} at rva {EXT_RVA:#x}; scratch {SCRATCH_LEN:#x} bytes at rva {SCRATCH_RVA:#x}")
print(f"blob {len(blob)} bytes of {STUB_MAX} at rva {STUB_RVA:#x}; hooks:")
for name, slot, rva, orig, riprel in hooks:
    print(f"  {name:30} rva {rva:#x} slot {slot:2} displaced {orig.hex()} tramp {offs['tr_' + name]:#x}")
