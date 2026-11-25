#!/usr/bin/env python3
"""Find the user's own Windows 10 IoT Enterprise LTSC 2021 media and pick the image to install. Read-only.

Usage:
  find_iso.py [--json] [--hash] PATH...        scan these files/folders (ISO files, or copied ISO contents)
  find_iso.py [--json] [--hash] --block-devices   (PS5 Linux only) mount every candidate partition read-only under
                                                  /run/ps5inst/scan/ and scan it; never the target, never NVMe
Exit code: 0 = exactly one usable source found, 2 = none, 3 = more than one (the installer asks which).
Nothing here writes to any disk. ISOs are read in place (UDF parser in lib/udf.py), never loop-mounted.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import udf  # noqa: E402
import wim  # noqa: E402

MAX_DEPTH = 3                  # how deep below each root to look for *.iso
MIN_ISO = 3 * 1024 ** 3        # LTSC 2021 x64 ISO is ~4.5 GiB; smaller files are not it
SKIP_DIRS = {"proc", "sys", "dev", "run", "tmp", "$RECYCLE.BIN", "System Volume Information", ".Trashes", ".Spotlight-V100"}


def sha256(read_iter):
    h = hashlib.sha256()
    for chunk in read_iter:
        h.update(chunk)
    return h.hexdigest()


def file_chunks(path, size=8 << 20):
    with open(path, "rb") as f:
        while chunk := f.read(size):
            yield chunk


def probe_iso(path, want_hash):
    r = {"kind": "iso", "path": str(path), "size": os.path.getsize(path)}
    try:
        r["iso9660_label"] = udf.iso9660_label(path)
        with udf.UdfImage(path) as img:
            r["udf_label"] = img.label
            for name in ("sources/install.wim", "sources/install.esd", "sources/install.swm"):
                ent = img.lookup(name)
                if ent is not None:
                    break
            else:
                r["error"] = "no sources/install.wim or install.esd: not a Windows install ISO"
                return r
            r["wim_path"] = name
            r["wim_size"] = ent["size"]
            r["has_efi_boot"] = img.lookup("efi/boot/bootx64.efi") is not None
            info, images = wim.read_images(lambda o, n: img.read_entry(ent, o, n))
    except (udf.UdfError, wim.WimError, OSError, ValueError) as e:
        r["error"] = f"{type(e).__name__}: {e}"
        return r
    _finish(r, info, images)
    if want_hash:
        r["sha256"] = sha256(file_chunks(path))
    return r


def probe_dir(path, want_hash):
    r = {"kind": "folder", "path": str(path)}
    src = Path(path) / "sources"
    for name in ("install.wim", "install.esd", "install.swm"):
        if (src / name).is_file():
            break
    else:
        r["error"] = "no sources/install.wim or install.esd"
        return r
    w = src / name
    r["wim_path"] = f"sources/{name}"
    r["wim_size"] = w.stat().st_size
    r["has_efi_boot"] = (Path(path) / "efi/boot/bootx64.efi").is_file()
    try:
        with open(w, "rb") as f:
            def read(o, n):
                f.seek(o)
                return f.read(n)
            info, images = wim.read_images(read)
    except (wim.WimError, OSError, ValueError) as e:
        r["error"] = f"{type(e).__name__}: {e}"
        return r
    _finish(r, info, images)
    if want_hash:
        r["sha256"] = sha256(file_chunks(w))
    return r


EDITION = None   # --edition (the answer file's edition=)


def _finish(r, info, images):
    r["wim"] = {k: info[k] for k in ("compression", "image_count", "total_parts", "pipable")}
    r["images"] = [{k: i[k] for k in ("index", "name", "EDITIONID", "ARCH", "BUILD", "SPBUILD", "LANGUAGES")}
                   for i in images]
    img, verdict, reasons = wim.pick_edition(info, images, EDITION)
    r["verdict"] = verdict
    r["reasons"] = reasons
    r["pick"] = img and {k: img[k] for k in ("index", "name", "EDITIONID", "BUILD", "SPBUILD", "LANGUAGES",
                                              "TOTALBYTES")}
    if verdict != "refuse" and not r.get("has_efi_boot"):
        r["reasons"].append("note: no efi/boot/bootx64.efi on the media (not needed: boot files come from the image)")


def candidates(root):
    root = Path(root)
    if root.is_file():
        yield root
        return
    if (root / "sources").is_dir():
        yield root
    base = len(root.parts)
    for dirpath, dirnames, filenames in os.walk(root):
        depth = len(Path(dirpath).parts) - base
        dirnames[:] = [] if depth >= MAX_DEPTH else [d for d in dirnames if d not in SKIP_DIRS and not d.startswith(".")]
        for fn in filenames:
            if fn.lower().endswith(".iso"):
                p = Path(dirpath) / fn
                try:
                    if p.stat().st_size >= MIN_ISO:
                        yield p
                except OSError:
                    pass
        for d in list(dirnames):
            if (Path(dirpath) / d / "sources").is_dir():
                yield Path(dirpath) / d


def probe(path, want_hash):
    return probe_iso(path, want_hash) if Path(path).is_file() else probe_dir(path, want_hash)


# ---- PS5 Linux only: block-device scan (untested on the console so far; the Mac tests use PATH mode) ----------------

READ_ONLY_FS = {"vfat": "vfat", "exfat": "exfat", "ntfs": "ntfs3", "ext4": "ext4", "ext3": "ext4", "ext2": "ext4",
                "btrfs": "btrfs", "iso9660": "iso9660", "udf": "udf"}


def block_device_roots(lsblk, exclude_serials=()):
    """From `lsblk -J -b -o NAME,PATH,TYPE,FSTYPE,MOUNTPOINT,TRAN,SERIAL,PKNAME` output, the partitions worth
    scanning: on USB disks only (the PS5's internal SSD is NVMe and is never looked at), not excluded, with a known
    file system. Already-mounted ones are scanned at their mount point."""
    out = []
    for disk in lsblk.get("blockdevices", []):
        if disk.get("type") != "disk" or disk.get("tran") != "usb":
            continue
        if (disk.get("serial") or "").strip() in exclude_serials:
            continue
        for part in disk.get("children", []) or [disk]:
            fs = part.get("fstype")
            if fs in READ_ONLY_FS:
                out.append({"dev": part["path"], "fstype": fs, "mountpoint": part.get("mountpoint"),
                            "disk_serial": (disk.get("serial") or "").strip()})
    return out


def mount_ro(dev, fstype, where):
    """Read-only mount. NTFS goes through ntfs3 with -o ro ONLY (a refused rw ntfs3 mount marks the volume dirty)."""
    os.makedirs(where, exist_ok=True)
    opts = "ro,noexec,nosuid,nodev"
    subprocess.run(["mount", "-t", READ_ONLY_FS[fstype], "-o", opts, dev, where], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def scan_block_devices(want_hash, exclude_serials):
    if sys.platform != "linux" or os.geteuid() != 0:
        sys.exit("--block-devices runs on the PS5's Linux as root only")
    lsblk = json.loads(subprocess.run(["lsblk", "-J", "-b", "-o", "NAME,PATH,TYPE,FSTYPE,MOUNTPOINT,TRAN,SERIAL,PKNAME"],
                                      check=True, capture_output=True, text=True).stdout)
    results, mounted = [], []
    try:
        for i, part in enumerate(block_device_roots(lsblk, exclude_serials)):
            where = part["mountpoint"]
            if not where:
                where = f"/run/ps5inst/scan/{i}"
                try:
                    mount_ro(part["dev"], part["fstype"], where)
                except subprocess.CalledProcessError:
                    continue
                mounted.append(where)
            for c in candidates(where):
                r = probe(c, want_hash)
                r["device"] = part["dev"]
                r["disk_serial"] = part["disk_serial"]
                results.append(r)
    finally:
        for m in reversed(mounted):
            subprocess.run(["umount", m], check=False)
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--block-devices", action="store_true")
    ap.add_argument("--exclude-serial", action="append", default=[], help="never scan this disk (the target)")
    ap.add_argument("--hash", action="store_true", help="also SHA-256 the ISO / WIM (reads the whole file)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--edition", help="edition to take (EDITIONID or image name); default: lib/wim.py EDITIONS order")
    a = ap.parse_args()
    global EDITION
    EDITION = a.edition
    if a.block_devices:
        results = scan_block_devices(a.hash, set(a.exclude_serial))
    else:
        if not a.paths:
            ap.error("give PATH... or --block-devices")
        results, seen = [], set()
        for root in a.paths:
            for c in candidates(root):
                key = os.path.realpath(c)
                if key not in seen:
                    seen.add(key)
                    results.append(probe(c, a.hash))
    usable = [r for r in results if r.get("verdict") in ("ok", "experimental")]
    if a.json:
        print(json.dumps({"results": results, "usable": len(usable)}, indent=2))
    else:
        for r in results:
            print(f"{r['kind']}: {r['path']}")
            if "error" in r:
                print(f"  skipped: {r['error']}")
                continue
            lab = r.get("udf_label") or r.get("iso9660_label")
            if lab:
                print(f"  label {lab}")
            print(f"  {r['wim_path']} {r['wim_size']:,} bytes, {r['wim']['compression']}, {r['wim']['image_count']} images")
            for i in r["images"]:
                print(f"    [{i['index']}] {i['name']} ({i['EDITIONID']}, arch {i['ARCH']}, "
                      f"10.0.{i['BUILD']}.{i['SPBUILD']}, {','.join(i['LANGUAGES'])})")
            print(f"  verdict: {r['verdict'].upper()}" + (f", image {r['pick']['index']}" if r.get("pick") else ""))
            for line in r["reasons"]:
                print(f"    - {line}")
            if "sha256" in r:
                print(f"  sha256 {r['sha256']}")
        print(f"{len(usable)} usable source(s).")
    sys.exit(0 if len(usable) == 1 else 2 if not usable else 3)


if __name__ == "__main__":
    main()
