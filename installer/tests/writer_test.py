#!/usr/bin/env python3
"""The write path (lib/writer.py) driven by a simulated system: no disk, no root, runs on the Mac.

Checks the exact command order of a full install, that the target is looked up again before every write command,
that PS5WIN is only ever mounted read-only, and that each dangerous change stops the run BEFORE the next write:
disk swapped mid-run, target now behind the internal SSD's PCI address, a mounted partition, a kept partition moved
(append), a new partition in the wrong place, a bad apply, an image file that is no longer the loop's backing file.
"""
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(HERE), str(HERE / "lib")]
import writer  # noqa: E402

WRITES = ("sgdisk -n", "sgdisk --zap-all", "sgdisk -o", "mkfs.vfat", "mkntfs")


def plan(fixture, serial, mode):
    p = subprocess.run([sys.executable, "-I", str(HERE / "plan_layout.py"), "--fixture", str(fixture), "--json",
                        "--target-serial", serial, "--mode", mode], capture_output=True, text=True)
    return json.loads(p.stdout)


class Fake:
    def __init__(self, inv, plan_out, tmp):
        self.inv, self.p, self.tmp = copy.deepcopy(inv), plan_out, Path(tmp)
        self.cmds, self.lookups, self.mounts, self.mounted_set = [], 0, [], set()
        self.hooks = {}          # command prefix -> function run after it
        self.apply_ok = True
        self.created = {}        # guid -> (dev, start, sectors)
        self.backing = None
        self.files = {}

    # System interface
    def run(self, cmd):
        line = " ".join(cmd)
        self.cmds.append(line)
        if cmd[:2] == ["sgdisk", "-n"]:
            n, s, e = cmd[2].split(":")
            guid = cmd[cmd.index("-u") + 1].split(":", 1)[1]
            disk = self.p["disk"]["path"]
            self.created[guid] = (f"{disk}{n}", int(s), int(e) - int(s) + 1)
        for k, f in self.hooks.items():
            if line.startswith(k):
                f(self)

    def settle(self):
        self.cmds.append("udevadm settle")

    def inventory(self):
        self.lookups += 1
        return copy.deepcopy(self.inv)

    def mounted(self, dev):
        return dev in self.mounted_set

    def resolve_partuuid(self, guid, timeout=20):
        return self.created.get(guid, (None,))[0]

    def geometry(self, dev):
        for d, s, n in self.created.values():
            if d == dev:
                return Path(self.p["disk"]["path"]).name, s, n

    def loop_backing(self, dev):
        return self.backing

    def apply_image(self, wim, index, adds, device):
        self.cmds.append(f"APPLY {device}")

    def mount(self, dev, where, fstype, ro):
        self.mounts.append((dev, fstype, ro))
        self.cmds.append(f"mount {fstype} {'ro' if ro else 'rw'} {dev}")

    def umount(self, where):
        self.cmds.append("umount")

    def copytree(self, src, dst):
        self.cmds.append("copy PS5BOOT")

    def read(self, path):
        for p, want in self.checks.items():
            if path.endswith(p):
                return Path(want).read_bytes() if self.apply_ok else b"wrong"
        return b"MZ..."

    def hash_tree(self, root):
        return writer.tree_hashes(self.boot)

    def write_file(self, path, data):
        self.files[path] = data

    def sync(self):
        pass


def setup(fixture, serial, mode, tmp):
    p = plan(fixture, serial, mode)
    inv = json.loads(Path(fixture).read_text())
    f = Fake(inv, p, tmp)
    work = Path(tmp)
    (work / "ps5win.conf").write_text("DISK_SERIAL=x\n")
    boot = work / "boot"
    (boot / "EFI/Boot").mkdir(parents=True, exist_ok=True)
    (boot / "EFI/Boot/bootx64.efi").write_bytes(b"efi")
    sysf = work / "SYSTEM"
    sysf.write_bytes(b"hive")
    f.checks = {"/Windows/System32/config/SYSTEM": sysf}
    f.boot = boot
    src = {"wim": "/x/install.wim", "index": 2}
    w = writer.Writer(f, p, src, [(sysf, "/Windows/System32/config/SYSTEM")], f.checks, boot, work,
                      lambda m: None, linux_conf=str(work / "etc-ps5win.conf"))
    return f, w, p


def expect_stop(w, f, text, forbidden):
    try:
        w.run_all()
    except writer.Stop as e:
        assert text in str(e), (text, str(e))
        for c in f.cmds:
            assert not any(c.startswith(x) for x in forbidden), ("ran after the stop", c)
        return str(e)
    raise AssertionError("did not stop")


def main():
    fx = HERE / "tests/fixtures"
    ok = 0
    with tempfile.TemporaryDirectory() as tmp:
        f, w, p = setup(fx / "two-disk.json", "T7TARGET0002", "erase", tmp)
        done = w.run_all()
        kinds = [c.split()[0] + (" " + c.split()[1] if c.startswith("sgdisk") else "") for c in f.cmds]
        assert kinds == ["sgdisk --backup=" + str(Path(tmp) / "gpt-before.bin"), "sgdisk --zap-all", "sgdisk -o",
                         "sgdisk -n", "sgdisk -n", "partprobe", "udevadm", "mkfs.vfat", "mkntfs", "APPLY",
                         "mount", "umount", "mount", "copy", "umount"], kinds
        nwrites = sum(any(c.startswith(x) for x in WRITES) for c in f.cmds)
        assert f.lookups >= nwrites + 3, (f.lookups, nwrites)        # before each write + formatting/apply/PS5BOOT
        win = w.devs["PS5WIN"]
        assert [m for m in f.mounts if m[0] == win] == [(win, "ntfs3", True)], f.mounts
        assert all(c.split()[-1] in w.devs.values() for c in f.cmds if c.startswith(("mkfs", "mkntfs")))
        assert f.files and done[-1] == "/etc/ps5win.conf"
        print(f"ok   erase: {len(f.cmds)} commands in order, {f.lookups} target look-ups for {nwrites} writes, "
              "PS5WIN mounted only as ntfs3 ro")
        ok += 1

    with tempfile.TemporaryDirectory() as tmp:
        f, w, p = setup(fx / "two-disk.json", "T7TARGET0002", "erase", tmp)
        def swap(s):
            for d in s.inv["disks"]:
                if d["serial"] == "T7TARGET0002":
                    d["serial"] = "SOMEOTHER"
        f.hooks["mkfs.vfat"] = swap
        expect_stop(w, f, "0 disks with serial", ["mkntfs", "APPLY"])
        print("ok   disk swapped after mkfs.vfat: stopped before mkntfs")
        ok += 1

    with tempfile.TemporaryDirectory() as tmp:
        f, w, p = setup(fx / "two-disk.json", "T7TARGET0002", "erase", tmp)
        def internal(s):
            for d in s.inv["disks"]:
                if d["serial"] == "T7TARGET0002":
                    d["syspath"] = "/sys/devices/pci0000:00/0000:00:01.1/0000:40:00.0/x"
        f.hooks["sgdisk --backup"] = internal
        expect_stop(w, f, "REFUSED", ["sgdisk --zap-all", "sgdisk -o", "sgdisk -n", "mkfs", "mkntfs", "APPLY"])
        print("ok   target suddenly behind PCI 0000:40:00.0: refused before the first write")
        ok += 1

    with tempfile.TemporaryDirectory() as tmp:
        f, w, p = setup(fx / "two-disk.json", "T7TARGET0002", "erase", tmp)
        f.mounted_set.add("/dev/sdb1")
        expect_stop(w, f, "mounted", ["sgdisk --zap-all", "sgdisk -o", "sgdisk -n", "mkfs", "mkntfs", "APPLY"])
        print("ok   erase with a mounted partition: stopped before wiping")
        ok += 1

    with tempfile.TemporaryDirectory() as tmp:
        f, w, p = setup(fx / "single-disk.json", "ONEDISK0004", "append", tmp)
        done = w.run_all()
        assert not any(c.startswith(("sgdisk --zap-all", "sgdisk -o")) for c in f.cmds), f.cmds
        print("ok   append: no wipe, no new table; kept partitions checked before each write")
        ok += 1

    with tempfile.TemporaryDirectory() as tmp:
        f, w, p = setup(fx / "single-disk.json", "ONEDISK0004", "append", tmp)
        def move(s):
            s.inv["disks"][0]["partitions"][1]["start"] += 2048
        f.hooks["sgdisk -n 3"] = move
        expect_stop(w, f, "kept partition /dev/sda2", ["sgdisk -n 4", "mkfs", "mkntfs", "APPLY"])
        print("ok   append: a kept partition moved after the first sgdisk: stopped before the second")
        ok += 1

    with tempfile.TemporaryDirectory() as tmp:
        f, w, p = setup(fx / "two-disk.json", "T7TARGET0002", "erase", tmp)
        real_run = f.run
        def misplace(cmd):
            real_run(cmd)
            if cmd[:2] == ["sgdisk", "-n"] and cmd[2].startswith("2:"):
                g = cmd[cmd.index("-u") + 1].split(":", 1)[1]
                d, s, n = f.created[g]
                f.created[g] = (d, s + 8, n)
        f.run = misplace
        expect_stop(w, f, "plan said", ["mkfs", "mkntfs", "APPLY"])
        print("ok   new partition not where the plan put it: stopped before formatting")
        ok += 1

    with tempfile.TemporaryDirectory() as tmp:
        f, w, p = setup(fx / "two-disk.json", "T7TARGET0002", "erase", tmp)
        f.apply_ok = False
        expect_stop(w, f, "files differ", ["copy PS5BOOT"])
        print("ok   apply wrote something else: stopped before PS5BOOT")
        ok += 1

    with tempfile.TemporaryDirectory() as tmp:
        f, w, p = setup(fx / "toshiba-image.json", "TOSHIBAIMG01", "append", tmp)
        assert p["plan"]["needs_gpt_move"] is True
        w.run_all()
        i_e = next(i for i, c in enumerate(f.cmds) if c.startswith("sgdisk -e"))
        i_n = next(i for i, c in enumerate(f.cmds) if c.startswith("sgdisk -n"))
        assert i_e < i_n and not any(c.startswith(("sgdisk --zap-all", "sgdisk -o")) for c in f.cmds), f.cmds
        assert [n["number"] for n in p["plan"]["new"]] == [4, 5]
        n4 = next(c for c in f.cmds if c.startswith("sgdisk -n 4:"))
        assert "-t 4:C12A7328-F81F-11D2-BA4B-00A0C93EC93B" in n4 and "-c 4:PS5BOOT" in n4, n4
        print("ok   Toshiba image layout (append on its own disk): backup GPT moved first, PS5BOOT #4 + PS5WIN #5")
        ok += 1

    with tempfile.TemporaryDirectory() as tmp:
        inv = {"disks": [{"name": "loop7", "path": "/dev/loop7", "size": 48 * 2**30, "tran": "loop",
                          "serial": "IMAGE-1234abcd", "model": "disk image", "ro": False, "pttype": None,
                          "ptuuid": None, "syspath": "/sys/devices/virtual/block/loop7", "pci": [], "partitions": []}]}
        fxp = Path(tmp) / "img.json"
        fxp.write_text(json.dumps(inv))
        pr = subprocess.run([sys.executable, "-I", str(HERE / "plan_layout.py"), "--fixture", str(fxp), "--json",
                             "--target-serial", "IMAGE-1234abcd", "--mode", "erase"], capture_output=True, text=True)
        assert pr.returncode == 4 and "not a USB disk" in pr.stdout, pr.stdout
        p = json.loads(subprocess.run([sys.executable, "-I", str(HERE / "plan_layout.py"), "--fixture", str(fxp),
                                       "--json", "--target-serial", "IMAGE-1234abcd", "--mode", "erase",
                                       "--allow-loop-image"], capture_output=True, text=True).stdout)
        f = Fake(inv, p, tmp)
        f.backing = "/run/other.img"
        (Path(tmp) / "ps5win.conf").write_text("x")
        w = writer.Writer(f, p, {"wim": "w", "index": 2}, [], {}, tmp, tmp, lambda m: None,
                          image_file="/run/ps5inst/test.img")
        expect_stop(w, f, "no longer backed by", ["sgdisk --zap-all", "sgdisk -o", "sgdisk -n", "mkfs"])
        print("ok   image mode: loop device no longer backed by our file: refused; a loop disk without "
              "--allow-loop-image is refused by the planner")
        ok += 1
    print(f"{ok} writer scenarios passed")


if __name__ == "__main__":
    main()
