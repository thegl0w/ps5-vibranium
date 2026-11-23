"""The installer's disk writes, in order, each one behind a fresh check of the target. PS5 Linux only (root).

Order: target check -> GPT backup -> partitions (sgdisk, exact sectors from the plan) -> new partitions checked by
PARTUUID -> mkfs.vfat PS5BOOT -> mkntfs PS5WIN -> one-pass wimlib apply in NTFS mode (unmounted volume, once) ->
read-only ntfs3 check of our files -> PS5BOOT files copied and hash-checked -> /etc/ps5win.conf (optional).
Before EVERY write command the target is looked up again (by serial, or by backing file for a disk image): same
device path and size, not the internal SSD, USB (or the installer's own loop image), nothing of it mounted (erase),
kept partitions byte-for-byte in place (append). Any mismatch stops the run before that command.
There is no rollback: a stop leaves the disk as far as it got, and the log says exactly how far.

All system access goes through a System object, so tests can drive the whole sequence with a simulated one.
"""
import hashlib
import os
import shutil
import subprocess
import time
from pathlib import Path


class Stop(Exception):
    """Refuse to go on. Nothing after this point was run."""


class System:
    """The real system (Linux, root)."""

    def __init__(self, log):
        self.log = log

    def run(self, cmd):
        self.log(f"RUN {' '.join(cmd)}")
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.stdout.strip():
            self.log("    " + p.stdout.strip().replace("\n", "\n    "))
        if p.returncode != 0:
            self.log(f"    exit {p.returncode}: {p.stderr.strip()}")
            raise Stop(f"{cmd[0]} failed (exit {p.returncode}): {p.stderr.strip()[-300:]}")

    def inventory(self):
        import plan_layout                      # read-only lsblk + /sys
        return plan_layout.live_inventory()

    def mounted(self, dev):
        return subprocess.run(["findmnt", "-rn", "-S", dev], capture_output=True).returncode == 0

    def resolve_partuuid(self, guid, timeout=20):
        link = Path(f"/dev/disk/by-partuuid/{guid}")
        end = time.time() + timeout
        while time.time() < end:
            if link.exists():
                return os.path.realpath(link)
            # no udev (minimal image, container): ask blkid, which reads the partition table itself
            q = subprocess.run(["blkid", "-t", f"PARTUUID={guid}", "-o", "device"], capture_output=True, text=True)
            devs = q.stdout.split()
            if len(devs) == 1:
                return devs[0]
            if len(devs) > 1:
                raise Stop(f"PARTUUID {guid} is on {len(devs)} devices")
            time.sleep(0.5)
        return None

    def settle(self):
        p = subprocess.run(["udevadm", "settle"], capture_output=True)
        self.log("RUN udevadm settle" + ("" if p.returncode == 0 else " (no udev here: skipped)"))

    def geometry(self, dev):
        """(parent disk name, start sector, sectors) of a partition device, from sysfs."""
        name = Path(dev).name
        base = Path(f"/sys/class/block/{name}")
        parent = Path(os.path.realpath(base)).parent.name
        return parent, int((base / "start").read_text()), int((base / "size").read_text())

    def loop_backing(self, dev):
        f = Path(f"/sys/block/{Path(dev).name}/loop/backing_file")
        return f.read_text().strip() if f.exists() else None

    def apply_image(self, wim, index, adds, device):
        from wimapi import Wim, EXTRACT_FLAG_NTFS
        self.log(f"APPLY image {index} of {wim} + {len(adds)} overlay entries -> {device} (NTFS mode)")
        last = [-1]
        def progress(done, total):
            pct = done * 100 // total if total else 0
            if pct != last[0]:
                last[0] = pct
                print(f"\r  Installing Windows: {pct:3d} %  ({done / 2**30:.1f} of {total / 2**30:.1f} GiB)",
                      end="", flush=True)
                if pct % 10 == 0:
                    self.log(f"    apply {pct} %")
        with Wim(wim) as w:
            w.overlay(index, adds=adds)
            w.extract_image(index, device, EXTRACT_FLAG_NTFS, progress=progress)
        print()

    def mount(self, dev, where, fstype, ro):
        os.makedirs(where, exist_ok=True)
        opts = ("ro," if ro else "") + "noexec,nosuid,nodev"
        self.run(["mount", "-t", fstype, "-o", opts, dev, where])

    def umount(self, where):
        self.run(["umount", where])

    def copytree(self, src, dst):
        shutil.copytree(src, dst, dirs_exist_ok=True)

    def read(self, path):
        return Path(path).read_bytes()

    def write_file(self, path, data):
        Path(path).write_text(data)

    def sync(self):
        os.sync()

    def hash_tree(self, root):
        return tree_hashes(root)


def tree_hashes(root, reader=None):
    out = {}
    for f in sorted(Path(root).rglob("*")):
        if f.is_file():
            data = reader(f) if reader else f.read_bytes()
            out[str(f.relative_to(root)).lower()] = hashlib.sha256(data).hexdigest()
    return out


class Writer:
    def __init__(self, sysm, plan_out, src, adds, checks, boot_tree, work, log, image_file=None, linux_conf=None):
        self.s, self.p, self.src, self.adds, self.checks = sysm, plan_out, src, adds, checks
        self.boot_tree, self.work, self.log = Path(boot_tree), Path(work), log
        self.image_file, self.linux_conf = image_file, linux_conf
        self.disk = plan_out["disk"]
        self.mode = plan_out["plan"]["mode"]
        self.new = plan_out["plan"]["new"]
        self.done = []

    # ---- the check before every write ----------------------------------------------------------------------------
    def verify_target(self, why):
        d = self.disk
        if self.image_file:
            if self.s.loop_backing(d["path"]) != str(self.image_file):
                raise Stop(f"before {why}: {d['path']} is no longer backed by {self.image_file}")
            inv_disk = next((x for x in self.s.inventory()["disks"] if x["path"] == d["path"]), None)
            size = inv_disk["size"] if inv_disk else os.path.getsize(self.image_file)
            if size != d["size"]:
                raise Stop(f"before {why}: image size changed")
            return
        import plan_layout
        inv = self.s.inventory()
        hits = [x for x in inv["disks"] if x.get("serial") == d["serial"]]
        if len(hits) != 1:
            raise Stop(f"before {why}: {len(hits)} disks with serial {d['serial']}")
        x = hits[0]
        if plan_layout.is_internal(x):
            raise Stop(f"before {why}: REFUSED, {plan_layout.is_internal(x)}")
        if x["path"] != d["path"] or x["size"] != d["size"] or x.get("tran") != "usb":
            raise Stop(f"before {why}: target changed ({x['path']} {x['size']} {x.get('tran')}; plan said "
                       f"{d['path']} {d['size']} usb)")
        new_guids = {n["guid"].lower() for n in self.new}
        if self.mode == "erase":
            m = [pp["path"] for pp in x["partitions"] if pp.get("mountpoint") or self.s.mounted(pp["path"])]
            if m:
                raise Stop(f"before {why}: partitions of the target are mounted: {', '.join(m)}")
        else:
            now = {pp["path"]: pp for pp in x["partitions"]}
            for k in self.p["plan"]["keep_detail"]:
                q = now.get(k["path"])
                if not q or (q["start"], q["sectors"], (q.get("partuuid") or "").lower()) != \
                        (k["start"], k["sectors"], (k.get("partuuid") or "").lower()):
                    raise Stop(f"before {why}: kept partition {k['path']} is not where the plan saw it")
            for pp in x["partitions"]:
                if (pp.get("partuuid") or "").lower() in new_guids and self.s.mounted(pp["path"]):
                    raise Stop(f"before {why}: new partition {pp['path']} is mounted")

    def step(self, name):
        self.log(f"STEP {name}")
        self.done.append(name)

    def write(self, cmd, why):
        self.verify_target(why)
        self.s.run(cmd)

    # ---- the sequence --------------------------------------------------------------------------------------------
    def partitions(self):
        dev = self.disk["path"]
        self.step("partition table")
        try:
            self.s.run(["sgdisk", f"--backup={self.work / 'gpt-before.bin'}", dev])
        except Stop:
            if self.mode == "append":
                raise
            self.log("    (no readable GPT before: nothing to back up)")
        if self.p["plan"].get("needs_gpt_move"):
            self.write(["sgdisk", "-e", dev], "moving the backup GPT to the end of the disk")
        if self.mode == "erase":
            self.write(["sgdisk", "--zap-all", dev], "wiping the partition table")
            self.write(["sgdisk", "-o", "-U", self.p["plan"]["disk_guid"], dev], "creating the GPT")
        for n in self.new:
            s, e = n["start"] // 512, (n["start"] + n["size"]) // 512 - 1
            self.write(["sgdisk", "-n", f"{n['number']}:{s}:{e}", "-t", f"{n['number']}:{n['type']}",
                        "-c", f"{n['number']}:{n['label']}", "-u", f"{n['number']}:{n['guid']}", dev],
                       f"creating {n['label']}")
        self.s.run(["partprobe", dev])
        self.s.settle()
        self.devs = {}
        parent = Path(dev).name
        for n in self.new:
            p = self.s.resolve_partuuid(n["guid"])
            if not p:
                raise Stop(f"{n['label']} (PARTUUID {n['guid']}) did not appear")
            got = self.s.geometry(p)
            want = (parent, n["start"] // 512, n["size"] // 512)
            if got != want:
                raise Stop(f"{n['label']} at {p}: {got}, plan said {want}")
            self.devs[n["label"]] = p
        self.verify_target("formatting")

    def filesystems(self):
        self.step("file systems")
        b, w = self.devs["PS5BOOT"], self.devs["PS5WIN"]
        for dev in (b, w):
            if self.s.mounted(dev):
                raise Stop(f"{dev} is mounted")
        self.write(["mkfs.vfat", "-F", "32", "-n", "PS5BOOT", b], "formatting PS5BOOT")
        self.write(["mkntfs", "--quick", "--label", "PS5WIN", w], "formatting PS5WIN")

    def apply(self):
        self.step("apply Windows (one pass, NTFS mode)")
        w = self.devs["PS5WIN"]
        self.verify_target("the apply")
        if self.s.mounted(w):
            raise Stop(f"{w} is mounted")
        self.s.apply_image(self.src["wim"], self.src["index"], self.adds, w)
        self.s.sync()

    def verify_windows(self):
        self.step("check Windows read-only")
        mnt = "/run/ps5inst/win-ro"
        try:
            self.s.mount(self.devs["PS5WIN"], mnt, "ntfs3", ro=True)     # READ-ONLY, never rw
        except Stop:
            self.log("    ntfs3 not available: trying ntfs-3g, read-only")
            self.s.mount(self.devs["PS5WIN"], mnt, "ntfs-3g", ro=True)
        try:
            bad = [p for p, want in self.checks.items()
                   if self.s.read(mnt + p) != Path(want).read_bytes()]
            if not self.s.read(mnt + "/Windows/System32/ntoskrnl.exe"):
                bad.append("ntoskrnl.exe")
        finally:
            self.s.umount(mnt)
        if self.s.mounted(self.devs["PS5WIN"]):
            raise Stop("PS5WIN still mounted after the check")
        if bad:
            raise Stop("files differ on PS5WIN after the apply: " + ", ".join(bad))

    def boot_partition(self):
        self.step("PS5BOOT files")
        mnt = "/run/ps5inst/boot"
        self.verify_target("writing PS5BOOT")
        self.s.mount(self.devs["PS5BOOT"], mnt, "vfat", ro=False)
        try:
            self.s.copytree(self.boot_tree, mnt)
            self.s.sync()
            want = tree_hashes(self.boot_tree)
            got = {k: v for k, v in self.s.hash_tree(mnt).items() if k in want}
        finally:
            self.s.umount(mnt)
        if got != want:
            raise Stop(f"PS5BOOT copy differs ({len(set(want) - set(got))} missing, "
                       f"{sum(got.get(k) != v for k, v in want.items())} different)")

    def linux_side(self):
        if not self.linux_conf:
            self.log("    /etc/ps5win.conf not written (image mode or --no-linux-conf)")
            return
        self.step("/etc/ps5win.conf")
        self.s.write_file(self.linux_conf, (self.work / "ps5win.conf").read_text())

    def run_all(self):
        for f in (self.partitions, self.filesystems, self.apply, self.verify_windows, self.boot_partition,
                  self.linux_side):
            f()
        self.s.sync()
        return self.done
