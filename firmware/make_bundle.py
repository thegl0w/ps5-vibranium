# SPDX-License-Identifier: MIT
"""Build a USB-only Windows bundle with private ACPI tables in reserved RAM."""
import os
from pathlib import Path
import struct
import subprocess
import sys

here = Path(__file__).resolve().parent
out = here.parent / "build"
repo = here.parents[1]
mac = repo / "ps5-hackintosh"
acpi = repo / "console-13.60/dump/acpi"
# The carve-out base must match the loader's vram.txt on the target drive: 0x370000000 = 4 GiB (releases up to 1.1),
# 0x330000000 = 5 GiB (release 1.2 on). The firmware (prepare.py -> Ps5WarmConfig.h PS5_CARVE_BASE, used
# by windows_bds.inc's map check) must have been compiled for the same base: checked below.
CARVES = {"0x370000000": "4 GiB", "0x330000000": "5 GiB"}
if os.environ.get("CARVE_BASE") not in CARVES:
    sys.exit("CARVE_BASE must be one of " + ", ".join(CARVES) + " (the live carve-out of the target drive).")
_cfg = (out / "edk2/UefiPayloadPkg/Library/PlatformBootManagerLib/Ps5WarmConfig.h").read_text()
if f"#define PS5_CARVE_BASE {os.environ['CARVE_BASE']}ULL" not in _cfg:
    sys.exit("The compiled firmware's PS5_CARVE_BASE differs from CARVE_BASE: rerun firmware/compile.sh with this CARVE_BASE.")
print("make_bundle: carve-out", CARVES[os.environ["CARVE_BASE"]], "at", os.environ["CARVE_BASE"])
os.environ.update(MIRROR="0", USBFIX="0")
base = 0x6eff0000
arena = bytearray()

def put(data):
    arena.extend(bytes(-len(arena) % 16))
    addr = base + len(arena)
    arena.extend(data)
    return addr

def table(data):
    t = bytearray(data)
    struct.pack_into("<I", t, 4, len(t))
    t[9] = 0
    t[9] = -sum(t) % 256
    return put(t)

# Keep the CPU declarations and AMD USB aperture. Omit the fictitious RTC and
# the Salina bridge's MMIO windows. The Toshiba uses AMD xHCI function 5.
dsdt = (mac / "opencore/ACPI/DSDT-PS5-v2.dsl").read_text()
dsdt = dsdt[:dsdt.index("    Scope (\\_SB.PCI0)")] + "}\n"
dsdt = dsdt.replace("0x007F, 0x0000, 0x0080", "0x0020, 0x0000, 0x0021")
start = dsdt.index("                DWordMemory")
end = dsdt.index("                DWordMemory", start + 1)
dsdt = dsdt[:start] + dsdt[end:]
start = dsdt.index("                QWordMemory")
dsdt = dsdt[:start] + dsdt[dsdt.index("            })", start):]
at = dsdt.index("            Name (CRS0")
dsdt = dsdt[:at] + """            Device (SBRG) { Name (_ADR, 0x00010001) Name (_STA, Zero) }
            Device (GPP0) {
                Name (_ADR, 0x00080001)
                Device (PSP0) { Name (_ADR, 2) Name (_STA, Zero) }
                Device (USB4) { Name (_ADR, 4) Name (_STA, Zero) }
            }
""" + dsdt[at:]
# acpi.sys (ACPIInitialize sleep-state scan) bugchecks 0xA5 (0x11, 0xD) when a hardware-reduced platform declares
# \\_S1, \\_S2 or \\_S3. Reproduced in KVM with a revision-5 FADT (build/vm12, VmFull5S3.efi); fixed by hiding S3.
start = dsdt.index("    Name (_S3, Package")
dsdt = dsdt[:start] + dsdt[dsdt.index("    Name (_S5, Package", start):]
assert not any(n in dsdt for n in ("_S1", "_S2", "_S3", "_S4"))
(out / "windows.dsl").write_text(dsdt)
subprocess.run(["iasl", "-p", str(out / "windows"), str(out / "windows.dsl")], check=True)
dsdt_addr = table((out / "windows.aml").read_bytes())

fadt = bytearray((acpi / "FACP.aml").read_bytes())
struct.pack_into("<I", fadt, 40, dsdt_addr)
struct.pack_into("<Q", fadt, 140, dsdt_addr)
# This board has no PC RTC or working ACPI PM timer. Its firmware advertises
# RAM at 0x9bf60 as every PM register. Describe a hardware-reduced platform.
struct.pack_into("<I", fadt, 112, (1 << 20) | (1 << 4) | (1 << 5))
struct.pack_into("<H", fadt, 109, 1 << 5)
fadt[48:96] = bytes(48)
fadt[148:244] = bytes(96)
# Console MADT plus one I/O APIC entry (id 0, GSI base 0) at the RAM page mkbundle already loads
# (IOAPIC_PAGE 0x7fd70000, 0x00170011 at +0x10). Windows' HAL rejects a hardware-reduced MADT with no
# I/O APIC (0x5C/0x200); the firmware patches the HAL's write-then-read probe so the RAM page passes
# (firmware/windows_halpatch.inc). Verified to reach WinPE in a PS5-like KVM guest (build/vm12).
madt = bytearray((acpi / "APIC.aml").read_bytes())
assert madt[:4] == b"APIC" and struct.unpack_from("<I", madt, 4)[0] == len(madt)
assert sum(madt) % 256 == 0
# Boot 21: WINCPUS=n (default 16) leaves only the first n processors enabled in the MADT, so the processor count can be chosen
# per bundle without touching the BCD on the disk (the installed system's BCD no longer has onecpu).
wincpus = int(os.environ.get("WINCPUS", "16"))
assert 1 <= wincpus <= 16
o, lapics = 44, 0
while o < len(madt):
    if madt[o] == 0:
        struct.pack_into("<I", madt, o + 4, 1 if madt[o + 3] < wincpus else 0)
        lapics += 1
    o += madt[o + 1]
assert lapics == 16
print(f"MADT: {wincpus} of 16 processors enabled")
madt += struct.pack("<BBBBII", 1, 12, 0, 0, 0x7fd70000, 0)
struct.pack_into("<I", madt, 4, len(madt))
mcfg = bytearray((acpi / "MCFG.aml").read_bytes())
assert len(mcfg) == 60 and mcfg[54:56] == b"\x00\x7f"
mcfg[55] = 0x20
tables = [table(x) for x in (fadt, madt, mcfg)]
# The Linux handoff turns off the IOMMU. Omit IVRS and the idle-state SSDT
# with southbridge port accesses for this short single-processor PoC.
snapshot = (acpi.parent / "acpi-region.bin").read_bytes()
def root(sig, width):
    offset = snapshot.index(sig)
    return table(snapshot[offset:offset+36] + b"".join(a.to_bytes(width, "little") for a in tables))

rsdt = root(b"RSDT", 4)
xsdt = root(b"XSDT", 8)
pos = 0x7fd8e014 - 0x7fd6f000
r = bytearray(snapshot[pos:pos+36])
assert r[:8] == b"RSD PTR " and r[15] == 2 and len(r) == 36
struct.pack_into("<I", r, 16, rsdt)
struct.pack_into("<Q", r, 24, xsdt)
r[8] = r[32] = 0
r[8] = -sum(r[:20]) % 256
r[32] = -sum(r) % 256
rsdp = put(r)
assert len(arena) < 65536
# Use the project's packer unchanged except for the private memory map and
# the extra ACPI segment. Keep its segment layout, headers and relocations.
src = (mac / "uefi/mkbundle.py").read_text()
def replace(old, new):
    global src
    assert src.count(old) == 1, old
    src = src.replace(old, new)
replace("RSDP = 0x7fd8e014", f"RSDP = {rsdp}")
replace("(0x00100000, MIRROR_LO)", "(0x00100000, 0x01000000)")
replace("(MIRROR_HI, 0x60000000)", "(0x40000000, 0x60000000)")
replace("(0x64829000, DMA_POOL[0])", f"(0x64829000, {base})")
replace("    DMA_POOL, ", f"    ({base}, 0x6f000000), (0x01000000, 0x40000000),\n    DMA_POOL, ")
replace("ap(0x00, 0x7f)", "ap(0x00, 0x20)")
# Boot 15: reserved resource HOBs with attributes present|initialized (0x3) are "untested memory" to the DXE core; its
# PromoteMemoryResource() turns them into free RAM when an allocation fails. Loading bootmgfw.efi at its preferred base
# 0x10000000 did exactly that, and Windows then used every reserved range (low 1 GiB, the ACPI arena, the fake I/O APIC
# page, Sony's firmware area) as ordinary RAM. Present-only (0x1) reserved ranges are never promoted.
replace("ATTR_RSVD = 0x00000003", "ATTR_RSVD = 0x00000001")
replace("(IOAPIC_PAGE, bytes(ioapic))]", f"(IOAPIC_PAGE, bytes(ioapic)), ({base}, bytes.fromhex('{arena.hex()}'))]")
sys.argv = ["mkbundle.py", str(out / "edk2/Build/UefiPayloadPkgX64/UniversalPayload.elf"), str(out / "windows.bundle")]
exec(compile(src, "windows-mkbundle", "exec"), {"__name__": "__main__"})
