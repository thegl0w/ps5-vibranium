#!/usr/bin/env python3
"""Write the BCD for PS5BOOT (\\EFI\\Microsoft\\Boot\\BCD) on Linux, without bcdboot/bcdedit. Or show any BCD store.

Usage:
  make_bcd.py --out BCD --boot-part GUID --win-part GUID --disk-guid GUID (--container FILE | --wim WIM --index N)
              [--locale en-US] [--description TEXT] [--no-firmware-pci]
  make_bcd.py --dump BCD          readable listing of a store (element names where known), and its structural check
The container is only an empty shell: the image's own Windows\\System32\\config\\BCD-Template (read from the WIM with
--wim), copied and cleared; nothing from it is kept. The store gets one Windows entry, default, no timeout:
bcdboot's settings (as found in the Toshiba install's store) plus testsigning, bootstatuspolicy IgnoreAllFailures,
recovery off, nocrashautoreboot, usefirmwarepcisettings (open question Q1: is that still set on today's SSD?).
"""
import argparse
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import bcd  # noqa: E402

OTYPE = {0x10100002: "Windows Boot Manager", 0x10200003: "Windows Boot Loader", 0x10200005: "memory test",
         0x10200004: "resume loader", 0x20100000: "settings (all apps)", 0x20200003: "settings (OS loaders)",
         0x20200004: "settings (resume)", 0x10100001: "firmware boot manager", 0x10300006: "legacy loader",
         0x30000000: "device options"}
KNOWN = {bcd.BOOTMGR: "{bootmgr}", bcd.GLOBALSETTINGS: "{globalsettings}", bcd.DBGSETTINGS: "{dbgsettings}",
         bcd.EMSSETTINGS: "{emssettings}", bcd.BADMEMORY: "{badmemory}", bcd.BOOTLOADERSETTINGS: "{bootloadersettings}",
         bcd.HYPERVISORSETTINGS: "{hypervisorsettings}"}


def guid(s):
    return "{" + str(uuid.UUID(s.strip("{}"))) + "}"


def dump(path):
    st = bcd.read(path)
    print(f"store {path}: " + ", ".join(f"{k}={v}" for k, v in st["_description"].items()))
    order = [bcd.BOOTMGR] + sorted(k for k in st if not k.startswith("_") and k != bcd.BOOTMGR)
    for g in order:
        if g not in st:
            continue
        o = st[g]
        print(f"\n{KNOWN.get(g, g)}  type 0x{o.get('type', 0):08x} ({OTYPE.get(o.get('type'), '?')})")
        for et in sorted(o["elements"]):
            v = o["elements"][et]
            if isinstance(v, dict) and v.get("kind") == "gpt-partition":
                v = f"partition {v['partition']} on GPT disk {v['disk']}"
            elif isinstance(v, list):
                v = " ".join(KNOWN.get(x, hex(x) if isinstance(x, int) else x) for x in v)
            elif isinstance(v, str):
                v = KNOWN.get(v, v)
            print(f"  {bcd.NAMES.get(et, f'{et:08x}'):24} {v}")
    probs = bcd.check(st)
    print("\ncheck: " + ("OK" if not probs else "; ".join(probs)))
    return probs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump")
    ap.add_argument("--out")
    ap.add_argument("--boot-part")
    ap.add_argument("--win-part")
    ap.add_argument("--disk-guid")
    ap.add_argument("--container")
    ap.add_argument("--wim")
    ap.add_argument("--index")
    ap.add_argument("--locale", default="en-US")
    ap.add_argument("--description", default="Windows 10 IoT Enterprise LTSC (PS5)")
    ap.add_argument("--no-firmware-pci", action="store_true")
    a = ap.parse_args()
    if a.dump:
        sys.exit(1 if dump(a.dump) else 0)
    if not (a.out and a.boot_part and a.win_part and a.disk_guid and (a.container or (a.wim and a.index))):
        ap.error("--out, --boot-part, --win-part, --disk-guid and --container or --wim/--index are needed")
    o = {"boot_part": guid(a.boot_part), "win_part": guid(a.win_part), "disk_guid": guid(a.disk_guid),
         "loader_guid": "{" + str(uuid.uuid4()) + "}", "locale": a.locale, "description": a.description,
         "firmware_pci": not a.no_firmware_pci}
    if len({o["boot_part"], o["win_part"], o["disk_guid"]}) != 3:
        sys.exit("boot partition, Windows partition and disk GUIDs must all differ")
    with tempfile.TemporaryDirectory() as td:
        container = a.container
        if not container:
            subprocess.run(["wimextract", a.wim, str(a.index), "/Windows/System32/config/BCD-Template",
                            f"--dest-dir={td}", "--no-acls", "--quiet"], check=True, stderr=subprocess.DEVNULL)
            container = str(Path(td) / "BCD-Template")
        bcd.build(container, a.out, bcd.osloader_objects(o))
    probs = bcd.check(bcd.read(a.out))
    if probs:
        print("BCD CHECK FAILED: " + "; ".join(probs))
        sys.exit(1)
    print(f"BCD written: {a.out} (Windows entry {o['loader_guid']}, boot manager on partition {o['boot_part']}, "
          f"Windows on {o['win_part']}, disk {o['disk_guid']})")


if __name__ == "__main__":
    main()
