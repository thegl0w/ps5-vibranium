# SPDX-License-Identifier: MIT
"""Prepare the private EDK2 source copy. Never modify the macOS checkout."""
from pathlib import Path
import hashlib
import zlib
import subprocess as sp
import sys

here = Path(__file__).resolve().parent
tree = here.parent / "build/edk2"
pci = tree / "MdeModulePkg/Bus/Pci/PciBusDxe/PciEnumeratorSupport.c"
src = pci.read_text()
# Until Boot 23 the second USB controller (20:00.4, the one with the Linux SSD) was hidden from the firmware too.
# From Boot 24 it is visible, so the Windows boot loader can read a Windows partition on that SSD. The firmware still
# starts only the approved partition (windows_bds.inc) and writes nothing to that disk.
old_guard = """  // Windows USB-only layout: skip the Salina bridge, its downstream bus,
  // the PSP and the Kingston's dedicated USB controller before any config read.
  if ((Bus == 0 && Device == 1 && Func == 1) || Bus == 0x40 ||
      (Bus == 0x20 && Device == 0 && (Func == 2 || Func == 4))) {
    return EFI_NOT_FOUND;
  }

"""
guard = """  // Windows USB-only layout: skip the Salina bridge, its downstream bus
  // and the PSP before any config read.
  if ((Bus == 0 && Device == 1 && Func == 1) || Bus == 0x40 ||
      (Bus == 0x20 && Device == 0 && Func == 2)) {
    return EFI_NOT_FOUND;
  }

"""
needle = "  Address = EFI_PCI_ADDRESS (Bus, Device, Func, 0);"
if old_guard in src:
    src = src.replace(old_guard, guard)
    pci.write_text(src)
elif guard not in src:
    idx = src.index(needle)
    pci.write_text(src[:idx] + guard + src[idx:])

bds = tree / "UefiPayloadPkg/Library/PlatformBootManagerLib/PlatformBootManager.c"
# The firmware checks the same WIM, BCD and SDI validated on the Mac.
# Independent per-MiB CRCs locate a short or corrupt EFI USB read.
# Boot 18 media: build/boot18/boot.wim = build/boot.wim (Boot 7 media, 9314e5ea...) with /ps5/{diag.cmd,target.exe,unattend.xml,...} from winpe/.
inputs = [("Wim", here.parent / "build/boot18/boot.wim", "31e59c1b107fa8e0ec63d1a9a142f079429c4d59a7f3db3fbf096476da76f004"),
          # The BCD as Windows Boot Manager left it after Boot 16 (build/BCD 456a6661... with \Description\KeyName = BCD00000000).
          ("Bcd", here.parent / "build/boot17/BCD", "3e08224190bc216315f8af70a041d588f7223745fc0d93d53be67b6d89a578b0"),
          ("Sdi", Path("/Volumes/CES_X64FREO_EN-US_DV9/boot/boot.sdi"), "cd2c00ce027687ce4a8bdc967f26a8ab82f651c9becd703658ba282ec49702bd")]
manifest = []
crc_header = bds.with_name("WindowsMediaCrc.h")
if not all(path.is_file() for _, path, _ in inputs) and crc_header.is_file():
    # The old WinPE media (build/boot18, ...) are gone; their CRC table only serves the WinPE install path. Keep the
    # table a previous build made from them (same inputs, same content) rather than failing.
    print(f"prepare: WinPE media missing, keeping {crc_header.name} from the last build")
    inputs = []
    manifest = None
for label, path, expected in inputs:
    digest = hashlib.sha256()
    chunks = []
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
            chunks.append(f"0x{zlib.crc32(chunk):08x}U")
    if digest.hexdigest() != expected:
        raise ValueError(f"Unexpected {label} image: {path}")
    manifest += [f"STATIC CONST UINT32 {label}Crc[] = {{" + ", ".join(chunks) + "};",
                 f"#define {label.upper()}_SIZE {path.stat().st_size}U"]
if manifest is not None:
    crc_header.write_text("\n".join(manifest) + "\n")
bds.with_name("WindowsTrace.h").write_text((here / "windows_trace.inc").read_text())
bds.with_name("WindowsHalPatch.h").write_text((here / "windows_halpatch.inc").read_text())
# every windows_kstub*.inc, one table (installer proposal 2026-11-21)
sp.run([sys.executable, str(here / "kstub_table.py"), "header", str(bds.with_name("WindowsKstub.h"))], check=True)
# KSTUB_DRAW=1: diagnostic build, the kernel stub draws its breadcrumb squares and stop codes (needs the picture in BAR0)
import os as _os
bds.with_name("WindowsKpat.h").write_text((here / "windows_kpat.inc").read_text())    # pattern stub (make_kpat.py)
bds.with_name("WindowsKstubInstall.h").write_text(("#define PS5_KSTUB_DRAW 1\n" if _os.environ.get("KSTUB_DRAW") else "")
                                                  + ("#define KSTUB_FORCE_PATTERN 1\n" if _os.environ.get("KSTUB_FORCE_PATTERN") == "1" else "")   # test: pattern stub even for table kernels
                                                  + ("#define KSTUB_DISABLED 1\n#define PS5_KSTUB_OPTIONAL 1\n" if _os.environ.get("KSTUB_OFF") == "1" else "")   # test: no kernel stub at all (measurement)
                                                  + (here / "windows_kstub_install.inc").read_text())
src = bds.read_text()
if "STATIC VOID WindowsBoot" not in src:
    start = src.index("// PS5 PATCH LOG")
    end = src.index("/**", start)
    src = src[:start] + (here / "windows_bds.inc").read_text() + "\n" + src[end:]
    start = src.index("  EfiBootManagerRefreshAllBootOption ();", src.index("PlatformBootManagerAfterConsole ("))
    end = src.index("\n}", start)
    src = src[:start] + "  WindowsBoot ();" + src[end:]
    bds.write_text(src.replace("  Ps5DumpLog ();", ""))

else:
    start = src.index("STATIC VOID WindowsBoot") if "#include <Guid/Acpi.h>" not in src else src.index("#include <Guid/Acpi.h>")
    end = src.index("/**", src.index("STATIC VOID WindowsBoot"))
    bds.write_text(src[:start] + (here / "windows_bds.inc").read_text() + "\n" + src[end:])

inf = bds.with_name("PlatformBootManagerLib.inf")
src = inf.read_text()
if "  gEfiAcpi20TableGuid" not in src:
    src = src.replace("[Guids]", "[Guids]\n  gEfiAcpi20TableGuid")
if "  gEfiBlockIoProtocolGuid" not in src:
    src = src.replace("[Protocols]", "[Protocols]\n  gEfiBlockIoProtocolGuid")
if "  gEfiPciIoProtocolGuid" not in src:
    src = src.replace("[Protocols]", "[Protocols]\n  gEfiPciIoProtocolGuid")
if "  gEfiUsbIoProtocolGuid" not in src:
    src = src.replace("[Protocols]", "[Protocols]\n  gEfiUsbIoProtocolGuid")
if "  gEfiDxeServicesTableGuid" not in src:
    src = src.replace("[Guids]", "[Guids]\n  gEfiDxeServicesTableGuid")
if "  gEfiPartitionInfoProtocolGuid" not in src:       # windows_bds.inc: PS5BOOT by GPT name (installer disks)
    src = src.replace("[Protocols]", "[Protocols]\n  gEfiPartitionInfoProtocolGuid")
if "  gEfiCpuArchProtocolGuid" not in src:
    src = src.replace("[Protocols]", "[Protocols]\n  gEfiCpuArchProtocolGuid")
if "\n  IoLib\n" not in src:
    src = src.replace("[LibraryClasses]", "[LibraryClasses]\n  IoLib")
inf.write_text(src)

# Boot 19: PS5 reset library (ICC power message) for ResetSystemRuntimeDxe only. Sources live in firmware/ps5reset.
# The payload is built from UefiPayloadPkg/Ps5Payload.dsc, whose default ResetSystemLib (Ps5Pkg, macOS project) triple-faults.
import shutil
lib = tree / "UefiPayloadPkg/Library/Ps5IccResetSystemLib"
if lib.exists():
    shutil.rmtree(lib)
lib.mkdir(parents=True, exist_ok=True)
# The library is the clean-room MIT implementation in cleanroom/ps5reset-mit (proven on the console 2026-11-22: Windows
# restart and shutdown). PS5RESET_DIR=<folder> picks another one; PS5RESET_DIR=firmware/ps5reset is the old GPL
# version (not for release). Every .c/.h/.inf/.inc in that folder is copied.
import os
alt = os.environ.get("PS5RESET_DIR") or str(here.parent / "cleanroom" / "ps5reset-mit")
if alt:
    for f in sorted(Path(alt).iterdir()):
        if f.suffix in (".c", ".h", ".inf", ".inc"):
            shutil.copyfile(f, lib / f.name)
    # warm restart (firmware/ps5warm): our own code, linked into the same library (its INF lists the files)
    for f in sorted((here / "ps5warm").iterdir()):
        if f.suffix in (".c", ".h", ".S"):
            shutil.copyfile(f, lib / f.name)
else:
    for name in ("Ps5IccResetSystemLib.c", "Ps5IccResetSystemLib.inf", "ps5_icc.inc", "ps5_varhook.inc"):
        shutil.copyfile(here / "ps5reset" / name, lib / name)
# Warm restart is experimental and parked: compiled in but inert unless PS5WARM=1 (then the constructor builds the block
# and the boot manager runs the 3a test when stage 1 asks for it). Default builds (dev, release) leave it off.
_warm = "#define PS5_WARM_ENABLED %d\n" % (1 if os.environ.get("PS5WARM") == "1" else 0)
(lib / "Ps5WarmConfig.h").write_text(_warm)
(tree / "UefiPayloadPkg/Library/PlatformBootManagerLib/Ps5WarmConfig.h").write_text(_warm)
# VRAM carve-out base the BDS map check protects (windows_bds.inc); must equal make_bundle.py's CARVE_BASE (it checks).
# Default 4 GiB (releases up to 1.1). 0x330000000 = 5 GiB (release 1.2 on).
_carve = os.environ.get("CARVE_BASE", "0x370000000")
if _carve not in ("0x370000000", "0x330000000"):
    raise SystemExit("prepare: CARVE_BASE must be 0x370000000 or 0x330000000")
print("prepare: carve-out base", _carve)
# Development disk identities (partition GUIDs of the dev Sabrent / old Toshiba) only in dev builds: PS5DEV=1.
(tree / "UefiPayloadPkg/Library/PlatformBootManagerLib/Ps5WarmConfig.h").write_text(
    _warm + "#define PS5_DEV_DISKS %d\n" % (1 if os.environ.get("PS5DEV") == "1" else 0)
    + "#define PS5_CARVE_BASE %sULL\n" % _carve)
_dev = tree / "UefiPayloadPkg/Library/PlatformBootManagerLib/WindowsDevDisks.h"
_dev.write_text((here / "dev_disks.inc").read_text() if os.environ.get("PS5DEV") == "1" else "// release build: no development disk identities\n")
print("prepare: dev disk identities", "IN (PS5DEV=1)" if os.environ.get("PS5DEV") == "1" else "out (release)")
print("prepare: warm restart", "ENABLED" if "1" in _warm else "off")
old_lib = tree / "UefiPayloadPkg/Library/Ps5ResetSystemLib"          # first attempt, wrong dsc
if old_lib.exists():
    shutil.rmtree(old_lib)
plain = "  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf\n"
wrong = ("  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf {\n    <LibraryClasses>\n"
         "      ResetSystemLib|UefiPayloadPkg/Library/Ps5ResetSystemLib/Ps5ResetSystemLib.inf\n  }\n")
override = ("  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf {\n    <LibraryClasses>\n"
            "      ResetSystemLib|UefiPayloadPkg/Library/Ps5IccResetSystemLib/Ps5IccResetSystemLib.inf\n  }\n")
dsc = tree / "UefiPayloadPkg/UefiPayloadPkg.dsc"                      # undo the first attempt there
src = dsc.read_text()
if wrong in src:
    dsc.write_text(src.replace(wrong, plain))
dsc = tree / "UefiPayloadPkg/Ps5Payload.dsc"
src = dsc.read_text()
if override not in src:
    assert src.count(plain) == 1
    dsc.write_text(src.replace(plain, override))

# Boot 31: no Ps5HeartbeatDxe in the Windows payload. It only draws (an "HB=" status line every 500 ms, and yellow, green
# and blue milestone squares at ExitBootServices), and those squares stayed on screen under the Windows logo.
for name in ("UefiPayloadPkg/Ps5Payload.fdf", "UefiPayloadPkg/Ps5Payload.dsc"):
    f = tree / name
    src = f.read_text()
    for line in ("  INF Ps5Pkg/Drivers/Ps5HeartbeatDxe/Ps5HeartbeatDxe.inf\n", "  Ps5Pkg/Drivers/Ps5HeartbeatDxe/Ps5HeartbeatDxe.inf\n"):
        if line in src:
            src = src.replace(line, "#" + line[1:])
    f.write_text(src)
