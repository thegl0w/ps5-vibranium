#!/usr/bin/env python3
"""BCD generator checks against the store bcdboot wrote for the Toshiba install (a real, booted store).

Usage: bcd_test.py <BCD-Template from the image> <reference BCD (build/boot21/efi-bcd/BCD.ondisk)> <scratch dir>
                   [<that disk's saved partition table (sfdisk dump)>]
 1. device encoding: every GPT partition device in the reference re-encodes to the same 88 bytes
 2. a generated store: structure check OK; values read back as given; the container file unchanged
 3. same format as bcdboot: for every element the generated store shares with the reference (boot manager, OS
    loader, settings objects), same registry type and length; the settings objects and the loader options we copy
    from bcdboot are byte-identical
 4. the checker catches a broken store (Windows entry removed)
"""
import hashlib
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
import bcd  # noqa: E402
from hive import Hive, walk, L  # noqa: E402

SAME_AS_BCDBOOT = ["inherit", "isolatedcontext", "allowedinmemorysettings", "systemroot", "nx", "bootmenupolicy",
                   "bootstatuspolicy", "usefirmwarepcisettings", "path"]   # nocrashautoreboot dropped in 1.1 (warm restart)


def raw_elements(path):
    out = {}
    with Hive(path) as h:
        for keypath, vals in walk(h):
            parts = keypath.split("\\")
            if len(parts) == 4 and parts[0] == "Objects" and parts[2] == "Elements":
                for n, t, raw in vals:
                    if n == "Element":
                        out[(parts[1].lower(), int(parts[3], 16))] = (t, raw)
    return out


def main(template, ref, scratch, sfdisk=None):
    scratch.mkdir(parents=True, exist_ok=True)
    rr = raw_elements(ref)
    n = 0
    for (g, et), (t, raw) in rr.items():
        if bcd.fmt_of(et) == "device" and len(raw) == 88:
            d = bcd.decode_device(raw)
            if d["kind"] == "gpt-partition":
                assert bcd.gpt_partition_device(d["partition"], d["disk"]) == raw, (g, hex(et))
                n += 1
    assert n >= 6, n
    st = bcd.read(ref)
    # the reference's boot manager device must be that disk's ESP: GUIDs from the saved partition table (dev file in
    # build/, read at run time so no disk identifier is written into this repo)
    if sfdisk and Path(sfdisk).is_file():
        text = Path(sfdisk).read_text()
        disk_guid = re.search(r"label-id:\s*([0-9A-Fa-f-]{36})", text).group(1).lower()
        esp = re.search(r"type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B,\s*uuid=([0-9A-Fa-f-]{36})", text, re.I).group(1).lower()
        dev = st[bcd.BOOTMGR]["elements"][bcd.E["device"]]
        assert dev == {"kind": "gpt-partition", "partition": esp, "disk": disk_guid}, dev
        print("ok   the reference's boot manager device = the ESP and disk GUID of its saved partition table")
    print(f"ok   device encoding: {n} GPT partition devices in the reference re-encode byte for byte")

    before = hashlib.sha256(Path(template).read_bytes()).hexdigest()
    o = {"boot_part": "{41082273-c2c4-4931-a884-0da26096156f}", "win_part": "{f569a950-1c18-4a73-8667-6c1a507f363c}",
         "disk_guid": "{4683bec9-76f0-4b8d-83cd-102cd7ca7d2c}", "loader_guid": "{11111111-2222-4333-8444-555555555555}",
         "locale": "en-US", "description": "Windows 10 IoT Enterprise LTSC (PS5)", "firmware_pci": True}
    gen = scratch / "BCD.test"
    objs = bcd.osloader_objects(o)
    bcd.build(template, gen, objs)
    assert hashlib.sha256(Path(template).read_bytes()).hexdigest() == before, "container changed"
    g = bcd.read(gen)
    assert not bcd.check(g), bcd.check(g)
    for guid, (otype, elems) in objs.items():
        assert g[guid]["type"] == otype
        for et, v in elems.items():
            got = g[guid]["elements"][et]
            if bcd.fmt_of(et) == "device":
                assert got == {"kind": "gpt-partition", "partition": o["win_part" if guid != bcd.BOOTMGR else "boot_part"].strip("{}"),
                               "disk": o["disk_guid"].strip("{}")}, got
            else:
                assert got == v, (guid, hex(et), got, v)
    assert set(k for k in g if not k.startswith("_")) == set(objs)
    print(f"ok   generated store: check OK, {sum(len(e) for _, e in objs.values())} elements read back as given, "
          "container unchanged")

    gr = raw_elements(gen)
    ref_loader = st[bcd.BOOTMGR]["elements"][bcd.E["default"]]       # the reference's Windows entry
    pairs = {bcd.BOOTMGR: bcd.BOOTMGR, o["loader_guid"]: ref_loader}
    for s in (bcd.GLOBALSETTINGS, bcd.DBGSETTINGS, bcd.EMSSETTINGS, bcd.BOOTLOADERSETTINGS, bcd.HYPERVISORSETTINGS):
        pairs[s] = s
    shared = same = 0
    for mine, theirs in pairs.items():
        for (gg, et), (t, raw) in gr.items():
            if gg != mine.lower() or (theirs.lower(), et) not in rr:
                continue
            t2, raw2 = rr[(theirs.lower(), et)]
            shared += 1
            assert t == t2, (mine, hex(et), t, t2)
            if bcd.fmt_of(et) in ("integer", "boolean", "device"):
                assert len(raw) == len(raw2), (mine, hex(et), len(raw), len(raw2))
            settings_obj = mine == theirs and mine != bcd.BOOTMGR
            if settings_obj or (mine == o["loader_guid"] and bcd.NAMES.get(et) in SAME_AS_BCDBOOT):
                if et == bcd.E["path"]:
                    assert raw.decode("utf-16-le").lower() == raw2.decode("utf-16-le").lower()
                else:
                    assert raw == raw2, (mine, hex(et), raw.hex(), raw2.hex())
                same += 1
    print(f"ok   format as bcdboot's: {shared} shared elements with the same type/length, {same} byte-identical "
          "(settings objects and the copied loader options)")

    broken = scratch / "BCD.broken"
    shutil.copyfile(gen, broken)
    with Hive(broken, write=True) as h:
        L.hivex_node_delete_child(h.h, h.node(f"Objects\\{o['loader_guid']}"))
        h.commit()
    probs = bcd.check(bcd.read(broken))
    assert any("missing object" in p for p in probs), probs
    print("ok   checker catches a store whose Windows entry is gone")


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2], Path(sys.argv[3]), sys.argv[4] if len(sys.argv) == 5 else None)
