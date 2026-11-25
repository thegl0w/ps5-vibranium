#!/usr/bin/env python3
"""Does the firmware carry a kernel stub for the ntoskrnl inside this install image? Read-only.

Usage: check_kernel.py <install.wim> <image index> [--firmware DIR]
Extracts Windows/System32/ntoskrnl.exe of that image into a temp dir (wimlib's wimextract; pass the WIM inside a
mounted ISO folder), reads its PE TimeDateStamp and SizeOfImage and compares them with every
firmware/windows_kstub*.inc (KSTUB_TIMESTAMP / KSTUB_IMAGE_SIZE). Without a table stub, checks whether the
firmware's pattern stub applies (lib/kpattern.py, the same four checks as the firmware).
Exit 0 = table stub or pattern stub (a "PATTERN STUB" line says which), 1 = neither.
"""
import argparse
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def pe_identity(data):
    e = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e:e + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    return struct.unpack_from("<I", data, e + 8)[0], struct.unpack_from("<I", data, e + 24 + 56)[0]


def stub_table(fwdir, table=None):
    """From kstub-table.txt ("<TimeDateStamp> <SizeOfImage> <file>", kstub_table.py list) if given or if no firmware
    sources are around (installer image), else from the firmware/windows_kstub*.inc sources."""
    tf = Path(table) if table else (HERE / "kstub-table.txt")
    if table or not Path(fwdir).is_dir():
        out = []
        for line in tf.read_text().splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[0].startswith("0x"):
                out.append((parts[2], int(parts[0], 16), int(parts[1], 16)))
        return out
    out = []
    for inc in sorted(Path(fwdir).glob("windows_kstub*.inc")):
        t = inc.read_text(errors="replace")
        ts = re.search(r"#define KSTUB_TIMESTAMP (0x[0-9a-fA-F]+)", t)
        sz = re.search(r"#define KSTUB_IMAGE_SIZE (0x[0-9a-fA-F]+)", t)
        if ts and sz:
            out.append((inc.name, int(ts.group(1), 16), int(sz.group(1), 16)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wim")
    ap.add_argument("index")
    ap.add_argument("--firmware", default=str(HERE.parent / "firmware"))
    ap.add_argument("--table", help="kstub-table.txt (the launch package's), instead of the firmware sources")
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as td:
        subprocess.run(["wimextract", a.wim, a.index, "/Windows/System32/ntoskrnl.exe", f"--dest-dir={td}",
                        "--no-acls", "--quiet"], check=True)
        data = (Path(td) / "ntoskrnl.exe").read_bytes()
    ts, size = pe_identity(data)
    print(f"ntoskrnl in image {a.index}: TimeDateStamp 0x{ts:08x}, SizeOfImage 0x{size:x}")
    match = None
    for name, sts, ssz in stub_table(a.firmware, a.table):
        hit = sts == ts and ssz == size
        print(f"  {'MATCH' if hit else '     '} {name}: 0x{sts:08x} / 0x{ssz:x}")
        match = match or (hit and name)
    if not match:
        sys.path.insert(0, str(HERE / "lib"))
        import kpattern
        why = kpattern.check(data)
        if why:
            print(f"no table stub, and the pattern checks fail ({', '.join(why)}): the firmware would stop at a red "
                  "screen. This Windows can't be used.")
            sys.exit(1)
        print("PATTERN STUB: no table stub, but entry, cave, APIC pointer and HAL probe are each found exactly once; "
              "the firmware patches this kernel with its pattern stub (experimental: not tested on the console with "
              "this kernel)")
        sys.exit(0)
    if match != "windows_kstub.inc" and not (a.table or not Path(a.firmware).is_dir()) and \
            not (Path(a.firmware) / "kstub_table.py").is_file():
        print(f"note: the stub exists ({match}) but this firmware tree builds windows_kstub.inc only")


if __name__ == "__main__":
    main()
