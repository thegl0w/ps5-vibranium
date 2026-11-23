#!/usr/bin/env python3
"""Plan the target disk for the PS5 Windows install. DRY RUN ONLY: this script never writes to any disk.

Usage:
  plan_layout.py --target-serial SERIAL --mode erase|append [--windows-gib N] [--iso-dev /dev/sdc1]
                 [--answer-file FILE] [--json] (--fixture FILE | --live)

  --fixture FILE  disk inventory as JSON (tests/fixtures/*.json), for testing on the Mac
  --live          PS5 Linux: read lsblk + /sys (read-only) for the inventory
  --mode erase    the whole target disk becomes PS5BOOT + PS5WIN (everything on it is lost)
  --mode append   keep every existing partition; add PS5BOOT + PS5WIN in the largest free space (the disk our Linux
                  image was flashed to, or any GPT disk with room)
  --answer-file   KEY=VALUE file (target_serial=..., mode=...); if both match it stands in for the typed yes

Exit codes: 0 plan made and confirmed (answer file matched), 1 plan made, needs the typed yes, 4 refused (reasons
printed), 2 usage. The plan lists the exact commands a real run would execute, marked WOULD RUN.
"""
import argparse
import json
import os
import subprocess
import sys
import uuid
from pathlib import Path

MIB = 1 << 20
GIB = 1 << 30
SECTOR = 512
BOOT_SIZE = 512 * MIB          # firmware trace files are 25 MiB each (9 slots filled the 300 MiB PS5BOOT on 2026-11-21)
MIN_WIN = 40 * GIB             # today's install lives in 40 GiB, games excluded
REC_WIN = 64 * GIB
GPT_TAIL = 34                  # backup GPT: header + 32 sectors of entries + 1
INTERNAL_PCI = "0000:40:00.0"  # PS5 internal SSD (Sony 104d:90eb): never, by path or by id
INTERNAL_IDS = {("0x104d", "0x90eb")}
TYPE_BASIC = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"   # Microsoft basic data: PS5WIN
# PS5BOOT must be an EFI System Partition: Windows Setup's specialize pass takes the firmware boot device as the system
# partition only if it is an ESP on GPT ("BCD: Failed to get system partition. Status: c0000452", Toshiba run 2,
# 2026-11-21). Firmware and run_stage.sh find PS5BOOT by GPT name + \ps5win.id and only check it is FAT32.
TYPE_ESP = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
LABELS = ("PS5BOOT", "PS5WIN")


def human(n):
    return f"{n / GIB:.1f} GiB" if n >= GIB else f"{n / MIB:.0f} MiB"


# ---- inventory ----------------------------------------------------------------------------------------------------

def live_inventory():
    if sys.platform != "linux":
        sys.exit("--live runs on the PS5's Linux only; use --fixture on the Mac")
    cols = "NAME,PATH,TYPE,SIZE,TRAN,SERIAL,MODEL,RO,PTTYPE,PTUUID,FSTYPE,LABEL,PARTLABEL,PARTUUID,MOUNTPOINT"
    lb = json.loads(subprocess.run(["lsblk", "-J", "-b", "-o", cols], check=True, capture_output=True, text=True).stdout)
    disks = []
    for d in lb["blockdevices"]:
        if d["type"] != "disk":
            continue
        syspath = os.path.realpath(f"/sys/block/{d['name']}/device")
        pci = []
        p = Path(syspath)
        while p != p.parent:
            if (p / "vendor").exists() and (p / "device").exists() and (p / "class").exists():
                pci.append({"addr": p.name, "vendor": (p / "vendor").read_text().strip(),
                            "device": (p / "device").read_text().strip()})
            p = p.parent
        parts = []
        for c in d.get("children") or []:
            base = f"/sys/block/{d['name']}/{c['name']}"
            parts.append({"name": c["name"], "path": c["path"], "fstype": c.get("fstype"), "label": c.get("label"),
                          "partlabel": c.get("partlabel"), "partuuid": c.get("partuuid"),
                          "mountpoint": c.get("mountpoint"),
                          "start": int(Path(base, "start").read_text()), "sectors": int(Path(base, "size").read_text())})
        lbs = int(Path(f"/sys/block/{d['name']}/queue/logical_block_size").read_text())
        alt = None
        if d.get("pttype") == "gpt" and lbs == 512:
            with open(d["path"], "rb") as f:            # read-only: the primary GPT header (LBA 1)
                f.seek(512)
                h = f.read(92)
            if h[:8] == b"EFI PART":
                alt = int.from_bytes(h[32:40], "little")     # where the backup header is
        disks.append({"name": d["name"], "path": d["path"], "size": int(d["size"]), "tran": d.get("tran"),
                      "logical_block_size": lbs, "gpt_backup_lba": alt,
                      "serial": (d.get("serial") or "").strip(), "model": (d.get("model") or "").strip(),
                      "ro": bool(int(d.get("ro") or 0)), "pttype": d.get("pttype"), "ptuuid": d.get("ptuuid"),
                      "syspath": syspath, "pci": pci,
                      "partitions": parts})
    return {"disks": disks}


# ---- safety -------------------------------------------------------------------------------------------------------

def is_internal(disk):
    if disk["name"].startswith("nvme") or disk.get("tran") == "nvme":
        return "it is an NVMe device (the PS5's internal SSD is NVMe; the installer only writes USB disks)"
    if INTERNAL_PCI in (disk.get("syspath") or ""):
        return f"it sits behind PCI {INTERNAL_PCI}, the PS5's internal SSD"
    for p in disk.get("pci") or []:
        if (p["vendor"].lower(), p["device"].lower()) in INTERNAL_IDS:
            return f"it sits behind PCI device {p['vendor']}:{p['device']} (Sony internal SSD)"
    return None


def linux_disk(inv):
    for d in inv["disks"]:
        for p in d["partitions"]:
            if p.get("mountpoint") in ("/", "/boot", "/boot/efi", "/run/archiso/bootmnt", "/run/ps5inst/media"):
                return d
    return None


def refusals(inv, disk, mode, iso_dev, allow_loop=False):
    r = []
    why = is_internal(disk)
    if why:
        return [f"REFUSED: {disk['path']}: {why}."]
    # --allow-loop-image: the installer's own disk-image file (losetup), for rehearsals; only /dev/loopN, erase only
    loop_ok = allow_loop and disk.get("tran") == "loop" and disk["path"].startswith("/dev/loop") and mode == "erase"
    if disk.get("tran") != "usb" and not loop_ok:
        r.append(f"{disk['path']} is not a USB disk (transport {disk.get('tran')!r}).")
    if disk.get("ro"):
        r.append(f"{disk['path']} is read-only.")
    if disk.get("logical_block_size", 512) != 512:
        r.append(f"{disk['path']} has {disk['logical_block_size']}-byte sectors; only 512 is handled.")
    holds_iso = iso_dev and any(p["path"] == iso_dev for p in disk["partitions"])
    ld = linux_disk(inv)
    if mode == "erase":
        if ld and ld["name"] == disk["name"]:
            r.append(f"{disk['path']} is the disk Linux is running from; use --mode append for it.")
        mounted = [p["path"] for p in disk["partitions"] if p.get("mountpoint")]
        if mounted:
            r.append(f"{disk['path']} has mounted partitions: {', '.join(mounted)}.")
        if holds_iso:
            r.append(f"{disk['path']} holds the Windows ISO ({iso_dev}); put the ISO on another disk.")
    else:
        if disk.get("pttype") != "gpt":
            r.append(f"{disk['path']} has no GPT partition table ({disk.get('pttype')!r}); append needs GPT.")
        if not disk.get("ptuuid"):
            r.append(f"{disk['path']}: GPT disk GUID unknown (the boot configuration names the disk by it).")
        have = [p for p in disk["partitions"] if (p.get("partlabel") or p.get("label")) in LABELS]
        if have:
            r.append(f"{disk['path']} already has {', '.join(p['path'] for p in have)} (PS5BOOT/PS5WIN): "
                     "reinstall is not supported yet.")
    if disk["size"] < BOOT_SIZE + MIN_WIN + 2 * MIB:
        r.append(f"{disk['path']} is too small ({human(disk['size'])}).")
    return r


# ---- layout -------------------------------------------------------------------------------------------------------

def align_up(x, a=MIB):
    return (x + a - 1) // a * a


def free_extents(disk):
    first = MIB
    last = (disk["size"] // SECTOR - GPT_TAIL) * SECTOR      # end of usable space (exclusive)
    used = sorted((p["start"] * SECTOR, (p["start"] + p["sectors"]) * SECTOR) for p in disk["partitions"])
    out, cur = [], first
    for s, e in used:
        if s > cur:
            out.append((cur, s))
        cur = max(cur, e)
    if last > cur:
        out.append((cur, last))
    return [(align_up(s), e) for s, e in out if align_up(s) < e]


def layout(disk, mode, windows_bytes):
    if mode == "erase":
        start, end = MIB, (disk["size"] // SECTOR - GPT_TAIL) * SECTOR
        keep = []
    else:
        ext = free_extents(disk)
        if not ext:
            return None, ["no free space on the disk"]
        start, end = max(ext, key=lambda e: e[1] - e[0])
        keep = disk["partitions"]
    end = end // MIB * MIB
    room = end - start
    if room < BOOT_SIZE + MIN_WIN:
        return None, [f"largest free space is {human(room)}; need {human(BOOT_SIZE + MIN_WIN)} "
                      f"({human(BOOT_SIZE)} PS5BOOT + at least {human(MIN_WIN)} for Windows)"]
    win = room - BOOT_SIZE if windows_bytes is None else min(windows_bytes, room - BOOT_SIZE)
    if win < MIN_WIN:
        return None, [f"Windows partition would be {human(win)}; minimum is {human(MIN_WIN)}"]
    taken = set()
    for p in keep:
        digits = p["name"][len(p["name"].rstrip("0123456789")):]
        taken.add(int(digits))
    free_nums = (n for n in range(1, 129) if n not in taken)
    boot = {"number": next(free_nums), "label": "PS5BOOT", "fs": "vfat (FAT32)", "type": TYPE_ESP,
            "start": start, "size": BOOT_SIZE, "guid": str(uuid.uuid4())}
    winp = {"number": next(free_nums), "label": "PS5WIN", "fs": "ntfs", "type": TYPE_BASIC,
            "start": start + BOOT_SIZE, "size": win, "guid": str(uuid.uuid4())}
    notes = []
    if win < REC_WIN:
        notes.append(f"Windows partition {human(win)} is below the recommended {human(REC_WIN)} (games need room)")
    return {"mode": mode, "keep": [p["path"] for p in keep],
            "keep_detail": [{k: p.get(k) for k in ("path", "start", "sectors", "partuuid")} for p in keep],
            "new": [boot, winp],
            # an image written to a bigger disk keeps its backup GPT right after its last partition: move it first
            "needs_gpt_move": mode == "append" and disk.get("gpt_backup_lba") is not None
                              and disk["gpt_backup_lba"] != disk["size"] // SECTOR - 1,
            "disk_guid": str(uuid.uuid4()) if mode == "erase" else disk["ptuuid"].lower()}, notes


def commands(disk, plan):
    dev = disk["path"]
    c = [f"# re-checked before every write: serial of {dev} is still {disk['serial']!r}, "
         + ("nothing on it mounted" if plan["mode"] == "erase" else "kept partitions unmoved")]
    if plan.get("needs_gpt_move"):
        c.append(f"sgdisk -e {dev}   # move the backup GPT to the end of the disk (it sits after the image's last partition)")
    if plan["mode"] == "erase":
        c.append(f"sgdisk --zap-all {dev}")
        c.append(f"sgdisk -o -U {plan['disk_guid']} {dev}")
    for p in plan["new"]:
        s, e = p["start"] // SECTOR, (p["start"] + p["size"]) // SECTOR - 1
        c.append(f"sgdisk -n {p['number']}:{s}:{e} -t {p['number']}:{p['type']} -c {p['number']}:{p['label']} "
                 f"-u {p['number']}:{p['guid']} {dev}")
    c.append(f"partprobe {dev}; udevadm settle")
    b, w = plan["new"]
    c.append(f"# by PARTUUID from here on (kernel names can move)")
    c.append(f"mkfs.vfat -F 32 -n PS5BOOT /dev/disk/by-partuuid/{b['guid']}")
    c.append(f"mkntfs --quick --label PS5WIN /dev/disk/by-partuuid/{w['guid']}")
    c.append(f"wimlib apply <install.wim> <index> + overlay -> /dev/disk/by-partuuid/{w['guid']}   "
             f"# one pass, NTFS mode (libntfs-3g, volume unmounted); afterwards only a read-only check")
    c.append(f"mount -t vfat /dev/disk/by-partuuid/{b['guid']} /run/ps5inst/boot   "
             f"# boot files + BCD + ps5win.id, then umount")
    return c


def confirmation(disk, mode):
    tail = disk["serial"][-4:]
    word = "ERASE" if mode == "erase" else "ADD"
    return f"{word} {tail}"


def read_answer(path):
    kv = {}
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            kv[k.strip()] = v.strip()
    return kv


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target-serial", required=True)
    ap.add_argument("--mode", choices=("erase", "append"), required=True)
    ap.add_argument("--windows-gib", type=int)
    ap.add_argument("--iso-dev")
    ap.add_argument("--answer-file")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--allow-loop-image", action="store_true", help="also accept a /dev/loopN disk image (erase only)")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--fixture")
    src.add_argument("--live", action="store_true")
    a = ap.parse_args()
    inv = json.loads(Path(a.fixture).read_text()) if a.fixture else live_inventory()

    matches = [d for d in inv["disks"] if d.get("serial") and d["serial"] == a.target_serial]
    for d in inv["disks"]:                               # the internal SSD is refused even by mistake of serial
        if d.get("serial") == a.target_serial and is_internal(d):
            print(f"REFUSED: {d['path']}: {is_internal(d)}.")
            sys.exit(4)
    if len(matches) != 1:
        print(f"REFUSED: {len(matches)} disks have serial {a.target_serial!r}; exactly one must.")
        for d in inv["disks"]:
            print(f"  {d['path']}: {d.get('model')!r} serial {d.get('serial')!r} {human(d['size'])} {d.get('tran')}")
        sys.exit(4)
    disk = matches[0]
    bad = refusals(inv, disk, a.mode, a.iso_dev, a.allow_loop_image)
    plan, notes = (None, []) if bad else layout(disk, a.mode, a.windows_gib and a.windows_gib * GIB)
    if not bad and plan is None:
        bad, notes = notes, []
    if bad:
        print(f"REFUSED: {disk['path']} ({disk.get('model')}, serial {disk['serial']}):")
        for line in bad:
            print(f"  - {line}")
        sys.exit(4)

    phrase = confirmation(disk, a.mode)
    confirmed = False
    if a.answer_file:
        kv = read_answer(a.answer_file)
        confirmed = kv.get("target_serial") == disk["serial"] and kv.get("mode") == a.mode
    out = {"disk": {k: disk.get(k) for k in ("path", "model", "serial", "size", "tran", "syspath")},
           "plan": plan, "notes": notes, "would_run": commands(disk, plan), "confirm_phrase": phrase,
           "confirmed_by_answer_file": confirmed}
    if a.json:
        print(json.dumps(out, indent=2))
    else:
        print("=" * 78)
        print(f"TARGET DISK: {disk['path']}  {disk.get('model')}  {human(disk['size'])}  serial {disk['serial']}  "
              f"({disk.get('tran')})")
        print(f"  path: {disk.get('syspath')}")
        print(f"MODE: {a.mode}" + ("  -- EVERYTHING ON THIS DISK WILL BE ERASED" if a.mode == "erase" else
                                    "  -- existing partitions are kept unchanged"))
        for p in disk["partitions"]:
            fate = "ERASED" if a.mode == "erase" else "kept"
            print(f"  {fate:6} {p['path']:12} {p.get('partlabel') or p.get('label') or '':10} {p.get('fstype') or '':6} "
                  f"{human(p['sectors'] * SECTOR):>10}  {p.get('mountpoint') or ''}")
        for p in plan["new"]:
            print(f"  NEW    #{p['number']:<3} {p['label']:10} {p['fs']:13} {human(p['size']):>10} at "
                  f"{p['start']:,}  PARTUUID {p['guid']}")
        for n in notes:
            print(f"  note: {n}")
        print("WOULD RUN (dry run, nothing executed):")
        for line in out["would_run"]:
            print(f"  {line}")
        if confirmed:
            print("CONFIRMED by the answer file (serial and mode match).")
        else:
            print(f"A real run stops here until the user types:  {phrase}")
        print("=" * 78)
    sys.exit(0 if confirmed else 1)


if __name__ == "__main__":
    main()
