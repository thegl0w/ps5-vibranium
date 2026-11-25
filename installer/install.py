#!/usr/bin/env python3
"""PS5 Windows installer, end to end.

--dry-run: every step runs for real except the writes to the target disk, which are listed (WOULD RUN). Anywhere.
Without --dry-run (PS5 Linux, root): the same steps, then the confirmation, then the writes (lib/writer.py: each
behind a fresh check of the target). --image-file writes into a NEW disk-image file instead of a disk (the first real
use should be this, booted in the KVM rig). The write path and the BCD have not run on the console yet.

Usage:
  install.py [--dry-run] --answers FILE (--media PATH... | --block-devices)
             (--fixture DISKS.json | --live | --image-file NEW.img [--image-gib 48])
             (--payload DIR | --payload-from-build) [--firmware-dir DIR] [--localtime FILE] [--workdir DIR]
             [--linux-conf /etc/ps5win.conf | --no-linux-conf] [--json]

  --answers        KEY=VALUE file on the stick: target_serial and mode (erase|append) are required; optional
                   windows_gib, computer_name, user_name, password, time_zone, locale, product_key (make_unattend.py)
  --media          ISO file(s) or copied ISO folders; the WIM steps need a folder (a mounted ISO). On the PS5 an ISO
                   file is loop-mounted read-only under /run/ps5inst/iso
  --block-devices  PS5 Linux: scan the USB disks read-only for the ISO (find_iso.py)
  --fixture/--live disk inventory for the layout plan (plan_layout.py)
  --payload DIR    a payload tree from build_payload.py (OUTDIR, holding ps5/ and payload.json); its MANIFEST is checked
  --payload-from-build  build it now from this repo's build outputs (development)
  --firmware-dir   where the GPU microcode is (default /usr/lib/firmware/amdgpu; .bin or .bin.zst)
  --workdir        scratch space (default: a new temp dir); never inside the repo; the report lands there
Exit: 0 complete, 1 a step failed or not confirmed (nothing written unless the write path had started), 2 usage.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "lib"))
PY = [sys.executable, "-I"]
FW_FILES = [f"cyan_skillfish2_{n}.bin" for n in ("ce", "me", "mec", "pfp", "rlc", "sdma", "sdma1")]
# PS5BOOT is 512 MiB. The firmware keeps at most 9 trace files (1 MiB) and 9 screen files (24 MiB): 225 MiB, slot 9
# reused once all exist (fix of 2026-11-22). Staged boot files are ~26 MiB; keep them under 212 MiB.
BOOT_LIMIT = 512 * 2**20 - 300 * 2**20


class Report:
    def __init__(self):
        self.steps, self.would = [], []

    def add(self, name, status, detail=""):
        self.steps.append({"step": name, "status": status, "detail": detail})
        mark = {"OK": "ok  ", "FAIL": "FAIL", "TODO": "todo", "SKIP": "skip", "WAIT": "wait"}[status]
        print(f"[{mark}] {name}" + (f": {detail}" if detail else ""), flush=True)

    @property
    def failed(self):
        return any(s["status"] == "FAIL" for s in self.steps)


def run(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


def read_kv(path):
    kv = {}
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            kv[k.strip()] = v.strip()
    return kv


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
    return h.hexdigest()


# ---- steps --------------------------------------------------------------------------------------------------------

# The VRAM carve-out the launch package was built for (its run_stage.sh checks /proc/iomem for it): 370000000 = 4 GiB
# (vram.txt 0x100000000), 330000000 = 5 GiB (vram.txt 0x140000000). The firmware in the package has the same base
# compiled in, so the console's vram.txt must match the package, never the other way round.
PACKAGE = Path(os.environ.get("PS5WIN_PACKAGE", HERE.parent / "package" / "deploy.tgz"))
CARVES = ("370000000", "330000000")


def expected_carve(package=None):
    import re
    import tarfile
    package = Path(package or PACKAGE)
    if not package.exists():
        return CARVES[0]
    with tarfile.open(package) as t:
        text = t.extractfile("run_stage.sh").read().decode()
    m = re.findall(r"\^([0-9a-f]+)-46fffffff : Reserved\$", text)
    if len(m) != 1 or m[0] not in CARVES:
        raise SystemExit(f"launch package {package}: no single known VRAM carve-out check in run_stage.sh ({m})")
    return m[0]


def preflight(r, a):
    if sys.platform != "linux":
        r.add("console checks", "SKIP", "not on the PS5 (kernel, crash kernel and VRAM carve-out are checked there)")
        return
    probs = []
    if Path("/sys/kernel/kexec_crash_size").exists() and Path("/sys/kernel/kexec_crash_size").read_text().strip() == "0":
        probs.append("no crash kernel memory (crashkernel=)")
    carve = expected_carve()
    if f"{carve}-46fffffff : Reserved" not in Path("/proc/iomem").read_text():
        probs.append(f"VRAM carve-out {carve}-46fffffff not reserved (vram.txt does not match the launch package)")
    r.add("console checks", "FAIL" if probs else "OK", "; ".join(probs))


def experimental_ok(kv):
    return kv.get("experimental", "").strip().lower() in ("yes", "1", "true")


def media(r, a, work, kv):
    cmd = PY + [str(HERE / "find_iso.py"), "--json"] + (["--block-devices"] if a.block_devices else a.media)
    if kv.get("edition"):
        cmd += ["--edition", kv["edition"]]
    p = run(cmd)
    try:
        res = json.loads(p.stdout)["results"]
    except (ValueError, KeyError):
        r.add("find the Windows media", "FAIL", (p.stdout + p.stderr).strip()[-300:])
        return None
    ok = ("ok", "experimental") if experimental_ok(kv) else ("ok",)
    usable = [x for x in res if x.get("verdict") in ok]
    if len(usable) != 1:
        why = f"{len(usable)} usable sources" + (f" ({len(res)} looked at)" if res else "")
        if any(x.get("verdict") == "experimental" for x in res) and not experimental_ok(kv):
            why += "; not the tested Windows (IoT Enterprise LTSC 2021): put experimental=yes in ps5win-answers.txt to try it"
        refused = [x for x in res if x.get("verdict") == "refuse"]
        if refused and refused[0].get("reasons"):
            why += "; " + refused[0]["reasons"][-1]
        r.add("find the Windows media", "FAIL", why)
        return None
    m = usable[0]
    pick = m["pick"]
    r.add("find the Windows media", "OK", f"{m['path']}: image {pick['index']} {pick['name']} 10.0.{pick['BUILD']}."
          f"{pick['SPBUILD']} {','.join(pick['LANGUAGES'])}" + (" (EXPERIMENTAL)" if m["verdict"] == "experimental" else ""))
    folder = Path(m["path"]) if m["kind"] == "folder" else None
    if folder is None and sys.platform == "linux" and os.geteuid() == 0:
        mnt = Path("/run/ps5inst/iso")
        mnt.mkdir(parents=True, exist_ok=True)
        if run(["mount", "-o", "loop,ro", "-t", "udf", m["path"], str(mnt)]).returncode == 0:
            folder = mnt
    wim = folder / m["wim_path"] if folder else None
    if wim is None and (sys.platform == "linux" or a.extract_wim_to):
        wim = extract_wim(r, a, m)              # no UDF loop mount here: copy install.wim out with our own reader
    if wim is None:
        r.add("open install.wim", "FAIL", "an ISO file needs a mounted folder on this machine (on the Mac: open the ISO "
              "in Finder and pass /Volumes/<label>, or --extract-wim-to DIR)")
        return None
    return {"wim": str(wim), "index": pick["index"], "language": (pick["LANGUAGES"] or ["en-US"])[0],
            "build": f"10.0.{pick['BUILD']}.{pick['SPBUILD']}", "media": m["path"], "edition": pick["EDITIONID"]}


def extract_wim(r, a, m):
    """Copy sources/install.wim out of the ISO file with lib/udf.py into the first place with room."""
    import udf
    need = m["wim_size"] + 512 * 2**20
    places = [a.extract_wim_to] if a.extract_wim_to else ["/var/tmp", str(Path(m["path"]).parent)]
    for place in places:
        try:
            free = shutil.disk_usage(place).free
        except OSError:
            continue
        if free < need:
            continue
        dest = Path(place) / "ps5inst-install.wim"
        t0 = time.time()
        def prog(done, total):
            print(f"\r  Copying install.wim out of the ISO: {done * 100 // total:3d} %", end="", flush=True)
        with udf.UdfImage(m["path"]) as img:
            img.extract(img.lookup(m["wim_path"]), dest, progress=prog)
        print()
        r.add("open install.wim", "OK", f"copied out of the ISO to {dest} ({m['wim_size'] / 2**30:.1f} GiB, "
              f"{time.time() - t0:.0f} s; removed after the install)")
        return dest
    r.add("open install.wim", "FAIL", f"the ISO cannot be mounted here and no place has {need / 2**30:.1f} GiB free "
          f"for a copy of install.wim (looked at {', '.join(places)})")
    return None


def kernel(r, src):
    p = run(PY + [str(HERE / "check_kernel.py"), src["wim"], str(src["index"])])
    line = next((l.strip() for l in p.stdout.splitlines() if "MATCH" in l or l.startswith("PATTERN STUB")), "")
    r.add("kernel stub for this Windows", "OK" if p.returncode == 0 else "FAIL",
          line.replace("MATCH ", "matches ") if p.returncode == 0 else p.stdout.strip().splitlines()[-1:])
    return p.returncode == 0


def plan(r, a, kv, work):
    if "target_serial" not in kv or kv.get("mode") not in ("erase", "append"):
        r.add("plan the target disk", "FAIL", "answer file needs target_serial= and mode=erase|append")
        return None
    cmd = PY + [str(HERE / "plan_layout.py"), "--json", "--target-serial", kv["target_serial"], "--mode", kv["mode"],
                "--answer-file", a.answers] + (["--fixture", a.fixture] if a.fixture else ["--live"]) + \
          (["--allow-loop-image"] if getattr(a, "image_file", None) else [])
    if kv.get("windows_gib"):
        cmd += ["--windows-gib", kv["windows_gib"]]
    p = run(cmd)
    if p.returncode not in (0, 1):
        r.add("plan the target disk", "FAIL", " ".join(p.stdout.split())[:400])
        return None
    out = json.loads(p.stdout)
    d, new = out["disk"], out["plan"]["new"]
    r.add("plan the target disk", "OK", f"{d['path']} {d['model']} serial {d['serial']}, {out['plan']['mode']}: "
          + ", ".join(f"#{n['number']} {n['label']} {n['size'] / 2**30:.1f} GiB" for n in new))
    if getattr(a, "image_file", None):
        r.add("confirmation", "OK", "target is a new disk-image file created by this run")
    elif out["confirmed_by_answer_file"]:
        r.add("confirmation", "OK", "answer file names this disk's serial and the mode")
    else:
        r.add("confirmation", "WAIT", f"a real run waits for the typed phrase {out['confirm_phrase']!r}")
    r.would += out["would_run"]
    (work / "plan.json").write_text(json.dumps(out, indent=1))
    return out


def hives(r, src, work, cert):
    p = run(PY + [str(HERE / "offline_reg.py"), src["wim"], str(src["index"]), str(work / "hives"), "--cert", str(cert)])
    last = (p.stdout.strip().splitlines() or [""])[-1]
    r.add("registry edits (copies of the image's hives)", "OK" if p.returncode == 0 else "FAIL",
          last if p.returncode == 0 else (p.stdout + p.stderr)[-300:])
    return p.returncode == 0


def unattend(r, a, src, work):
    out = work / "unattend.xml"
    cmd = PY + [str(HERE / "make_unattend.py"), "--out", str(out), "--answers", a.answers, "--language",
                src["language"], "--hive", str(work / "hives/SOFTWARE"), "--json", "--edition-id", src["edition"]]
    if a.localtime:
        cmd += ["--localtime", a.localtime]
    p = run(cmd)
    if p.returncode != 0:
        r.add("unattend.xml", "FAIL", p.stdout.strip())
        return None
    s = json.loads(p.stdout)
    r.add("unattend.xml", "OK", f"{s['computer_name']}, user {s['user_name']} (password {s['password'].split()[0]}), "
          f"{s['time_zone']}, {s['locale']}, product key "
          f"{s['product_key'] if s['product_key'].startswith('edition') else s['product_key'].split()[0]}")
    return out


def payload(r, a, work):
    if a.payload_from_build:
        out = work / "payload"
        p = run(PY + [str(HERE / "build_payload.py"), str(out)])
        if p.returncode != 0:
            r.add("C:\\ps5 payload", "FAIL", p.stdout.strip()[-400:])
            return None
    else:
        out = Path(a.payload)
    root = out / "ps5"
    man = root / "setup/MANIFEST.sha256"
    if not man.is_file():
        r.add("C:\\ps5 payload", "FAIL", f"no {man}")
        return None
    bad = []
    lines = man.read_text().splitlines()
    for line in lines:
        h, rel = line.split("  ", 1)
        f = root / rel.replace("\\", "/")
        if not f.is_file() or sha256(f) != h:
            bad.append(rel)
    info = json.loads((out / "payload.json").read_text())
    if info.get("test_only_firmware"):
        bad.append("payload was built with --firmware-dir (test only)")
    r.add("C:\\ps5 payload", "FAIL" if bad else "OK",
          ("bad: " + ", ".join(bad[:5])) if bad else f"{len(lines)} files, {info['bytes'] / 2**20:.1f} MiB, hashes match")
    return None if bad else root


def microcode(r, a, work):
    src = Path(a.firmware_dir)
    out = work / "fw"
    out.mkdir(exist_ok=True)
    missing = []
    for name in FW_FILES:
        plain, zst = src / name, src / (name + ".zst")
        if plain.is_file():
            shutil.copyfile(plain, out / name)
        elif zst.is_file():
            try:
                from compression import zstd           # Python 3.14+
                (out / name).write_bytes(zstd.decompress(zst.read_bytes()))
            except ImportError:
                if run(["zstd", "-d", "-q", "-f", "-o", str(out / name), str(zst)]).returncode != 0:
                    missing.append(name + ".zst (no zstd)")
        else:
            missing.append(name)
    lic = next((f for f in (src / "LICENSE.amdgpu", Path("/usr/share/licenses/linux-firmware-amdgpu/LICENSE.amdgpu"),
                             Path("/usr/share/licenses/linux-firmware/LICENSE.amdgpu")) if f.is_file()), None)
    if lic:
        shutil.copyfile(lic, out / "LICENSE.amdgpu")           # AMD's terms travel with the microcode
    else:
        missing.append("LICENSE.amdgpu (AMD's licence must go with the microcode)")
    r.add("GPU microcode (from Linux's firmware)", "FAIL" if missing else "OK",
          ("missing in " + str(src) + ": " + ", ".join(missing)) if missing else f"7 files + LICENSE.amdgpu from {src}")
    return None if missing else out


def rehearse(r, src, work, adds, checks):
    from wimapi import Wim, EXTRACT_FLAG_NO_ACLS, EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE
    wim = Path(src["wim"])
    before = (wim.stat().st_size, wim.stat().st_mtime_ns)
    out = work / "rehearsal"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir()
    t0 = time.time()
    with Wim(wim) as w:
        w.overlay(src["index"], adds=adds)
        w.extract_paths(src["index"], out, list(checks), EXTRACT_FLAG_NO_ACLS | EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE)
    bad = [p for p, want in checks.items() if (out / Path(p).name).read_bytes() != Path(want).read_bytes()]
    after = (wim.stat().st_size, wim.stat().st_mtime_ns)
    if after != before:
        bad.append("install.wim changed")
    r.add("one-pass apply rehearsal (in memory, extracted to the work dir)", "FAIL" if bad else "OK",
          ("differs: " + ", ".join(bad)) if bad else f"{len(adds)} overlay entries; {len(checks)} files read back "
          f"byte for byte in {time.time() - t0:.1f} s; install.wim untouched")
    r.would.append(f"apply image {src['index']} with the same overlay onto PS5WIN (wimlib NTFS mode, unmounted, once)")
    return not bad


def bootfiles(r, src, work):
    from wimapi import Wim, EXTRACT_FLAG_NO_ACLS
    tmp, stage = work / "bootsrc", work / "ps5boot"
    for d in (tmp, stage):
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir()
    with Wim(src["wim"]) as w:
        w.extract_paths(src["index"], tmp, ["/Windows/Boot/EFI", "/Windows/Boot/Fonts", "/Windows/Boot/Resources"],
                        EXTRACT_FLAG_NO_ACLS)
    mb = stage / "EFI/Microsoft/Boot"
    shutil.copytree(tmp / "Windows/Boot/EFI", mb)                 # what bcdboot does
    shutil.copytree(tmp / "Windows/Boot/Fonts", mb / "Fonts")
    shutil.copytree(tmp / "Windows/Boot/Resources", mb / "Resources")
    (stage / "EFI/Boot").mkdir(parents=True)
    shutil.copyfile(mb / "bootmgfw.efi", stage / "EFI/Boot/bootx64.efi")
    shutil.rmtree(tmp)
    files = [f for f in stage.rglob("*") if f.is_file()]
    size = sum(f.stat().st_size for f in files)
    ok = (mb / "bootmgfw.efi").is_file() and size < BOOT_LIMIT
    r.add("PS5BOOT boot files (staged)", "OK" if ok else "FAIL",
          f"{len(files)} files, {size / 2**20:.1f} MiB, bootmgfw.efi from image {src['index']}")
    r.would.append("copy the staged PS5BOOT tree to the new FAT32 partition")
    return stage if ok else None


def make_store(r, src, plan_out, work):
    boot, win = plan_out["plan"]["new"]
    out = work / "ps5boot/EFI/Microsoft/Boot/BCD"
    p = run(PY + [str(HERE / "make_bcd.py"), "--out", str(out), "--wim", src["wim"], "--index", str(src["index"]),
                  "--boot-part", boot["guid"], "--win-part", win["guid"], "--disk-guid", plan_out["plan"]["disk_guid"],
                  "--locale", src["language"]])
    if p.returncode != 0:
        r.add("BCD for PS5BOOT", "FAIL", (p.stdout + p.stderr).strip()[-300:])
        return False
    r.add("BCD for PS5BOOT", "OK", "generated and structure-checked (boot manager on PS5BOOT, Windows on PS5WIN, "
          "testsigning); not yet booted on the console")
    return True


def records(r, src, plan_out, work):
    d = plan_out["disk"]
    boot, win = plan_out["plan"]["new"]
    ident = (f"# PS5 Windows install record (written by the installer; read by the firmware and run_stage.sh)\n"
             f"INSTALLER=ps5-installer-dev\nDATE={time.strftime('%Y-%m-%dT%H:%M:%S')}\nDISK_SERIAL={d['serial']}\n"
             f"DISK_GUID={plan_out['plan']['disk_guid']}\n"
             f"BOOT_PARTUUID={boot['guid']}\nWIN_PARTUUID={win['guid']}\nWINDOWS_BUILD={src['build']}\n"
             f"IMAGE_INDEX={src['index']}\n")
    conf = (f"DISK_SERIAL={d['serial']}\nBOOT_PARTUUID={boot['guid']}\nWIN_PARTUUID={win['guid']}\n"
            f"WINDOWS_BUILD={src['build']}\n")
    (work / "ps5boot/ps5win.id").write_text(ident.replace("\n", "\r\n"))
    (work / "ps5win.conf").write_text(conf)
    r.add("install records", "OK", "\\ps5win.id (PS5BOOT) and /etc/ps5win.conf staged")
    r.would.append("write /etc/ps5win.conf on the Linux side (format of the run_stage.sh proposal)")


def build_overlay(work, pl, fw, u):
    tree = work / "overlay-ps5"
    shutil.rmtree(tree, ignore_errors=True)
    shutil.copytree(pl, tree)
    shutil.copytree(fw, tree / "gpu/fw")
    adds = [(work / "hives/SYSTEM", "/Windows/System32/config/SYSTEM"),
            (work / "hives/SOFTWARE", "/Windows/System32/config/SOFTWARE"),
            (tree, "/ps5"),
            (u, "/Windows/Panther/unattend.xml"),
            (tree / "setup/SetupComplete.cmd", "/Windows/Setup/Scripts/SetupComplete.cmd")]
    checks = {"/Windows/System32/config/SYSTEM": work / "hives/SYSTEM",
              "/Windows/System32/config/SOFTWARE": work / "hives/SOFTWARE",
              "/Windows/Panther/unattend.xml": u,
              "/Windows/Setup/Scripts/SetupComplete.cmd": tree / "setup/SetupComplete.cmd",
              "/ps5/gpu/wddm/ps5wddm.sys": tree / "gpu/wddm/ps5wddm.sys",
              "/ps5/gpu/fw/cyan_skillfish2_rlc.bin": tree / "gpu/fw/cyan_skillfish2_rlc.bin",
              "/ps5/gpu/fw/LICENSE.amdgpu": tree / "gpu/fw/LICENSE.amdgpu"}
    return adds, checks


def image_target(a, work):
    """Create the disk-image file, attach it (losetup), and describe it to the planner. Returns (loop dev, fixture)."""
    img = Path(a.image_file).resolve()
    if img.exists():
        sys.exit(f"{img} exists: image mode only writes a new file")
    if a.image_gib < 41:
        sys.exit("--image-gib: at least 41 (512 MiB PS5BOOT + 40 GiB Windows)")
    with open(img, "wb") as f:
        f.truncate(a.image_gib * 2**30)                          # sparse
    loop = run(["losetup", "-f", "-P", "--show", str(img)]).stdout.strip()
    if not loop.startswith("/dev/loop"):
        img.unlink()
        sys.exit("losetup failed")
    inv = {"disks": [{"name": Path(loop).name, "path": loop, "size": img.stat().st_size, "tran": "loop",
                      "serial": "IMAGE-" + hashlib.sha256(str(img).encode()).hexdigest()[:8], "model": "disk image",
                      "ro": False, "pttype": None, "ptuuid": None, "syspath": os.path.realpath(f"/sys/block/{Path(loop).name}"),
                      "pci": [], "partitions": []}]}
    fx = work / "image-inventory.json"
    fx.write_text(json.dumps(inv))
    return loop, fx, inv["disks"][0]["serial"]


def confirm(plan_out, image):
    if image:
        return True                                              # a brand-new file of our own
    if plan_out["confirmed_by_answer_file"]:
        return True
    d = plan_out["disk"]
    print("\n" + "=" * 78)
    print(f"TARGET: {d['path']}  {d['model']}  {d['size'] / 2**30:.1f} GiB  serial {d['serial']}")
    print("MODE:   " + ("EVERYTHING ON THIS DISK WILL BE ERASED" if plan_out["plan"]["mode"] == "erase"
                        else "existing partitions are kept; two partitions are added"))
    print(f"Type exactly  {plan_out['confirm_phrase']}  and Enter to write; anything else stops with nothing written.")
    try:
        with open("/dev/tty") as tty:
            answer = tty.readline().strip()
    except OSError:
        return False
    return answer == plan_out["confirm_phrase"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--answers", required=True)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--media", nargs="+")
    g.add_argument("--block-devices", action="store_true")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--fixture")
    g.add_argument("--live", action="store_true")
    g.add_argument("--image-file", help="real run into a NEW disk-image file (losetup) instead of a disk")
    ap.add_argument("--image-gib", type=int, default=48)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--payload")
    g.add_argument("--payload-from-build", action="store_true")
    ap.add_argument("--firmware-dir", default="/usr/lib/firmware/amdgpu")
    ap.add_argument("--localtime")
    ap.add_argument("--extract-wim-to", help="where to copy install.wim out of an ISO file when it cannot be mounted")
    ap.add_argument("--workdir")
    ap.add_argument("--linux-conf", default="/etc/ps5win.conf")
    ap.add_argument("--no-linux-conf", action="store_true")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    real = not a.dry_run
    if not (a.fixture or a.live or a.image_file):
        ap.error("one of --fixture, --live, --image-file is needed")
    if real:
        if sys.platform != "linux" or os.geteuid() != 0:
            sys.exit("a real run is for the PS5's Linux, as root (use --dry-run elsewhere)")
        if a.fixture:
            sys.exit("a real run never uses a fixture: --live (a disk) or --image-file (a new image file)")
    elif a.image_file:
        sys.exit("--image-file is a real run into a new file; for a rehearsal without writes use --dry-run --fixture")
    work = Path(a.workdir or tempfile.mkdtemp(prefix="ps5inst-")).resolve()
    if HERE.parent in work.parents or work == HERE.parent:
        sys.exit("--workdir must be outside the repo")
    work.mkdir(parents=True, exist_ok=True)
    kv = read_kv(a.answers)
    loop = None
    if a.image_file:
        loop, fx, serial = image_target(a, work)
        a.fixture, kv["target_serial"], kv["mode"] = str(fx), serial, "erase"
    r = Report()
    src = None
    print(f"PS5 Windows installer, {'REAL RUN' if real else 'DRY RUN (nothing is written to any disk)'}; work dir {work}")
    try:
        preflight(r, a)
        src = media(r, a, work, kv)
        p = adds = checks = st = None
        if src and kernel(r, src):
            p = plan(r, a, kv, work)
            pl = payload(r, a, work) if p else None
            if p and pl and hives(r, src, work, pl / "gpu/wddm/ps5test.cer"):
                u = unattend(r, a, src, work)
                fw = microcode(r, a, work)
                if u and pl and fw:
                    adds, checks = build_overlay(work, pl, fw, u)
                    rehearse(r, src, work, adds, checks)
                st = bootfiles(r, src, work)
                if st and make_store(r, src, p, work):
                    records(r, src, p, work)
                else:
                    st = None
        if not real:
            r.add("write path", "SKIP", "dry run: nothing written (install.py without --dry-run on the console writes)")
            if r.would:
                print("WOULD RUN on the target (dry run, nothing executed):")
                for line in r.would:
                    print(f"  {line}")
        elif r.failed or not (p and adds and st):
            r.add("write path", "FAIL", "a check above failed: nothing was written")
        elif not confirm(p, bool(a.image_file)):
            r.add("confirmation", "FAIL", "not confirmed: nothing was written")
        else:
            sys.path.insert(0, str(HERE))
            import writer
            logf = work / "install.log"
            def log(m):
                with open(logf, "a") as f:
                    f.write(time.strftime("%H:%M:%S ") + m + "\n")
            w = writer.Writer(writer.System(log), p, src, adds, checks, st, work, log,
                              image_file=str(Path(a.image_file).resolve()) if a.image_file else None,
                              linux_conf=None if (a.image_file or a.no_linux_conf) else a.linux_conf)
            try:
                done = w.run_all()
                r.add("write path", "OK", " -> ".join(done) + f"; log {logf}")
            except writer.Stop as e:
                r.add("write path", "FAIL", f"stopped during '{w.done[-1] if w.done else 'start'}': {e}; nothing "
                      f"after that was run; log {logf}")
    finally:
        if loop:
            run(["losetup", "-d", loop])
        if src and Path(src["wim"]).name == "ps5inst-install.wim":
            Path(src["wim"]).unlink(missing_ok=True)
    report = {"work": str(work), "real": real, "steps": r.steps, "would_run": r.would}
    (work / "report.json").write_text(json.dumps(report, indent=1))
    word = "RUN" if real else "DRY RUN"
    print((f"{word} FAILED" if r.failed else f"{word} COMPLETE") + f"; report {work / 'report.json'}")
    if a.json:
        print(json.dumps(report, indent=1))
    sys.exit(1 if r.failed else 0)


if __name__ == "__main__":
    main()
