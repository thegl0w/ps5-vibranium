#!/usr/bin/env python3
"""PS5 Windows installer, the automatic run on the installer disk (tty1, root). Called by ps5win-autostart.sh when
this disk's Linux has no /etc/ps5win.conf yet. The user never types.

1. Wait for PS5ISO (/mnt/ps5iso) and find the user's ISO on it; nothing else is looked at.
2. Target = the disk this Linux booted from (the one holding "/"), and PS5ISO must be on that same disk.
   Append mode: PS5BOOT + PS5WIN go into its free space; every existing partition is kept. The planner still refuses
   NVMe / the internal SSD / anything not USB.
3. Full dry run first (install.py --dry-run --live): media, kernel stub, plan, registry, unattend, payload, microcode,
   apply rehearsal, PS5BOOT, BCD. Any failure: say why on screen, stop, nothing written.
4. The exact disk and what will happen are shown, with a countdown; any key cancels (stays in Linux, nothing written).
5. The real run (install.py --live, confirmed by the generated answer file naming this disk's serial and append).
6. Logs to PS5ISO (/mnt/ps5iso/logs/, readable from the Mac). On success /etc/ps5win.conf exists (written by the
   installer) and Windows is started: exec /opt/ps5win/ps5win-start.sh.

Testing: autorun.py --dry-run --fixture DISKS.json --media DIR --payload DIR [--firmware-dir DIR] --countdown 0
(the boot disk is then the fixture disk with a partition mounted at "/").
"""
import argparse
import json
import os
import select
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE / "lib")]
ISO_MOUNT = Path("/mnt/ps5iso")
START = Path("/opt/ps5win/ps5win-start.sh")


class Screen:
    def __init__(self, logfile):
        self.logfile = logfile
        self.f = open(logfile, "a") if logfile else None

    def say(self, text=""):
        print(text, flush=True)
        if self.f:
            self.f.write(text + "\n")
            self.f.flush()

    def raw(self, chunk):
        sys.stdout.write(chunk)
        sys.stdout.flush()
        if self.f:
            self.f.write(chunk)
            self.f.flush()


def log_place(dry):
    stamp = time.strftime("%Y%m%d-%H%M%S")
    for d in ([] if dry else [ISO_MOUNT / "logs"]) + [Path("/var/log/ps5win"), Path(f"/tmp/ps5inst-logs")]:
        try:
            d.mkdir(parents=True, exist_ok=True)
            f = d / f"install-{stamp}.txt"
            f.touch()
            return f
        except OSError:
            continue
    return None


def wait_for_iso_mount(scr, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        try:
            os.listdir(ISO_MOUNT)                     # touching it triggers the automount
            if os.path.ismount(ISO_MOUNT):
                return True
        except OSError:
            pass
        time.sleep(2)
    return False


def iso_source_devices(findmnt_out):
    """Block devices among findmnt's SOURCE lines for the PS5ISO mount point. With x-systemd.automount there are two
    lines: the autofs placeholder ("systemd-1") and the real device (first Toshiba run, 2026-11-21)."""
    return sorted({l.strip() for l in findmnt_out.splitlines() if l.strip().startswith("/dev/")})


def user_answers(path):
    """The user's optional answer file as KEY=VALUE (same rules as install.read_kv); {} if absent or unreadable."""
    kv = {}
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError:
        return kv
    for line in lines:
        line = line.strip().lstrip("\ufeff")
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            kv[k.strip()] = v.strip()
    return kv


def blank_password(path):
    """After a successful install, empty every active `password=` line of the user's answer file on PS5ISO (exFAT,
    readable on any computer). The key stays as an empty `password=` with a comment line above it.
    Comments, other keys, line endings and a UTF-8 BOM are kept. Written to a temporary file in the same folder,
    flushed, then renamed over the original. Returns "blanked", "none" (no password set) or "failed: <reason>".
    The password itself is never shown or logged."""
    try:
        raw = path.read_bytes()
    except OSError as e:
        return f"failed: cannot read ({e.strerror})"
    bom = b"\xef\xbb\xbf" if raw.startswith(b"\xef\xbb\xbf") else b""
    text = raw[len(bom):].decode("utf-8", errors="surrogateescape")
    out, changed = [], False
    for line in text.splitlines(keepends=True):
        body = line.rstrip("\r\n")
        end = line[len(body):]
        s = body.strip()
        if s and not s.startswith("#") and "=" in s and s.split("=", 1)[0].strip() == "password":
            if s.split("=", 1)[1].strip():
                # the parsers have no inline comments: the note goes on its own line, the key stays empty
                body, changed = ("# The installer blanked this password after using it. Set your password in "
                                 "Windows." + (end or "\n") + "password="), True
        out.append(body + end)
    if not changed:
        return "none"
    tmp = path.with_name(path.name + ".tmp")
    try:
        with open(tmp, "wb") as f:
            f.write(bom + "".join(out).encode("utf-8", errors="surrogateescape"))
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except OSError as e:
        try:
            tmp.unlink()
        except OSError:
            pass
        return f"failed: cannot write ({e.strerror})"
    return "blanked"


def boot_disk(inv):
    for d in inv["disks"]:
        for p in d["partitions"]:
            if p.get("mountpoint") == "/":
                return d
    return None


def run_install(scr, args):
    """Run install.py, showing its output (progress lines included) and logging it. Returns (exit code, report)."""
    cmd = [sys.executable, "-I", str(HERE / "install.py")] + args
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    while True:
        chunk = p.stdout.read1(4096) if hasattr(p.stdout, "read1") else p.stdout.read(1)
        if not chunk:
            break
        scr.raw(chunk.decode("utf-8", "replace"))
    rc = p.wait()
    work = args[args.index("--workdir") + 1]
    rep = Path(work) / "report.json"
    return rc, (json.loads(rep.read_text()) if rep.is_file() else None)


def countdown(scr, secs):
    if secs <= 0:
        return True
    tty = None
    try:
        tty = open("/dev/tty", "rb", buffering=0)
        import termios
        import tty as ttymod
        old = termios.tcgetattr(tty)
        ttymod.setcbreak(tty)
    except (OSError, ImportError):
        old = None
    try:
        for left in range(secs, 0, -1):
            scr.raw(f"\r  Installing in {left:2d} s. Press any key to cancel. ")
            if tty and select.select([tty], [], [], 1)[0]:
                tty.read(1)
                scr.say("\n  Cancelled. Nothing was written. Linux keeps running.")
                return False
            if not tty:
                time.sleep(1)
        scr.say("")
        return True
    finally:
        if tty and old is not None:
            import termios
            termios.tcsetattr(tty, termios.TCSADRAIN, old)
        if tty:
            tty.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true", help="test mode: stop after the dry run (needs --fixture)")
    ap.add_argument("--fixture")
    ap.add_argument("--media", default=str(ISO_MOUNT))
    ap.add_argument("--payload", default=str(HERE / "payload"))
    ap.add_argument("--firmware-dir", default="/usr/lib/firmware/amdgpu")
    ap.add_argument("--countdown", type=int, default=30)
    ap.add_argument("--workdir", default="/run/ps5inst/work")
    ap.add_argument("--localtime")
    a = ap.parse_args()
    if a.dry_run != bool(a.fixture):
        sys.exit("--dry-run and --fixture go together (test mode); the real run reads the disks itself")
    if not a.dry_run and (sys.platform != "linux" or os.geteuid() != 0):
        sys.exit("the automatic install runs on the PS5's Linux, as root")
    scr = Screen(log_place(a.dry_run))
    scr.say("=" * 60)
    scr.say("PS5 Windows installer")
    scr.say("=" * 60)
    if scr.logfile:
        scr.say(f"Log: {scr.logfile}")

    # 1. the ISO
    if not a.dry_run and a.media == str(ISO_MOUNT) and not wait_for_iso_mount(scr):
        scr.say("The PS5ISO partition is not mounted. Nothing was done.")
        return 1
    user = user_answers(Path(a.media) / "ps5win-answers.txt")
    exp = user.get("experimental", "").strip().lower() in ("yes", "1", "true")
    p = subprocess.run([sys.executable, "-I", str(HERE / "find_iso.py"), "--json", a.media]
                       + (["--edition", user["edition"]] if user.get("edition") else []), capture_output=True, text=True)
    found = json.loads(p.stdout)["results"] if p.stdout.strip().startswith("{") else []
    usable = [x for x in found if x.get("verdict") in (("ok", "experimental") if exp else ("ok",))]
    if len(usable) != 1:
        scr.say("")
        if not found:
            scr.say("No Windows ISO on the PS5ISO partition.")
        for x in found:
            scr.say(f"  {Path(x['path']).name}: " + (x.get("error") or f"{x.get('verdict')}: "
                                                      + "; ".join(x.get("reasons", []))))
        if len(usable) > 1:
            scr.say("More than one usable ISO: leave exactly one on PS5ISO.")
        if not exp and any(x.get("verdict") == "experimental" for x in found):
            scr.say("This Windows is not the tested one. To try it anyway (experimental), put the line")
            scr.say("  experimental=yes")
            scr.say("into ps5win-answers.txt on PS5ISO.")
        scr.say("Copy your own Windows 10 IoT Enterprise LTSC 2021 (x64) ISO onto PS5ISO from the Mac, then start")
        scr.say("this disk again. Nothing was written.")
        return 1
    iso = usable[0]
    scr.say(f"Windows media: {Path(iso['path']).name}, image {iso['pick']['index']} "
            f"({iso['pick']['name']}, 10.0.{iso['pick']['BUILD']}.{iso['pick']['SPBUILD']})")
    if iso["verdict"] == "experimental":
        scr.say("EXPERIMENTAL: not the tested Windows (IoT Enterprise LTSC 2021). The firmware patches its kernel by")
        scr.say("pattern; Windows Update stays off. If it fails, reinstall with the tested ISO.")

    # 2. the target: the disk this Linux runs from, with PS5ISO on it
    if a.fixture:
        inv = json.loads(Path(a.fixture).read_text())
    else:
        import plan_layout
        inv = plan_layout.live_inventory()
    disk = boot_disk(inv)
    if not disk:
        scr.say("Cannot tell which disk this Linux runs from. Nothing was written.")
        return 1
    labels = {(q.get("label") or q.get("partlabel") or "").upper() for q in disk["partitions"]}
    if "PS5ISO" not in labels:
        scr.say(f"{disk['path']} (this Linux's disk) has no PS5ISO partition: not an installer disk. Nothing written.")
        return 1
    if a.media == str(ISO_MOUNT) and not a.dry_run:
        src = subprocess.run(["findmnt", "-n", "-o", "SOURCE", str(ISO_MOUNT)], capture_output=True, text=True)
        devs = iso_source_devices(src.stdout)
        if len(devs) != 1 or devs[0] not in [q["path"] for q in disk["partitions"]]:
            scr.say(f"PS5ISO ({', '.join(devs) or 'no device'}) is not on this Linux's disk {disk['path']}. "
                    "Nothing was written.")
            return 1

    # 3. dry run with a generated answer file (target = this disk, append)
    work = Path(a.workdir)
    work.mkdir(parents=True, exist_ok=True)
    answers = work.parent / "answers.txt"
    extra = Path(a.media) / "ps5win-answers.txt"          # optional user settings (computer name, time zone, ...)
    lines = []
    if extra.is_file():
        lines = [l for l in extra.read_text().splitlines() if not l.strip().startswith(("target_serial", "mode"))]
    answers.write_text("\n".join(lines + [f"target_serial={disk['serial']}", "mode=append"]) + "\n")
    common = ["--answers", str(answers), "--media", a.media, "--payload", a.payload, "--firmware-dir", a.firmware_dir]
    if a.localtime:
        common += ["--localtime", a.localtime]
    common += ["--fixture", a.fixture] if a.fixture else ["--live"]
    scr.say("\nChecking everything first (nothing is written yet):")
    rc, rep = run_install(scr, ["--dry-run", "--workdir", str(work / "dry")] + common)
    if rc != 0 or not rep:
        scr.say("\nA check failed (see above). Nothing was written. Linux keeps running.")
        return 1
    plan = json.loads((work / "dry/plan.json").read_text())
    if plan["plan"]["mode"] != "append" or plan["disk"]["serial"] != disk["serial"]:
        scr.say("The plan is not an append to this Linux's own disk. Nothing was written.")
        return 1

    # 4. show exactly what happens, countdown
    d = plan["disk"]
    scr.say("")
    scr.say("Windows will be installed on THIS disk (the one this Linux runs from):")
    scr.say(f"  {d['path']}  {d['model']}  {d['size'] / 2**30:.0f} GiB  serial {d['serial']}")
    scr.say("  kept unchanged: " + ", ".join(plan["plan"]["keep"]))
    for n in plan["plan"]["new"]:
        scr.say(f"  new: {n['label']}  {n['size'] / 2**30:.1f} GiB")
    scr.say("")
    scr.say("Anti-cheat: don't play online games with anti-cheat (EasyAntiCheat, BattlEye, Vanguard) on this")
    scr.say("install; modified graphics files can get accounts banned.")
    if a.dry_run:
        scr.say("\nTEST MODE: stopping before the countdown and the writes.")
        return 0
    if not countdown(scr, a.countdown):
        return 2

    # 5. the real run
    scr.say("Installing (about 15-30 minutes). Do not switch the console off.")
    rc, rep = run_install(scr, ["--workdir", str(work / "real")] + common)
    for name in ("install.log", "report.json"):
        f = work / "real" / name
        if f.is_file() and scr.logfile:
            try:
                (scr.logfile.parent / f"{scr.logfile.stem}-{name}").write_bytes(f.read_bytes())
            except OSError:
                pass
    if rc != 0:
        scr.say("\nThe install stopped (see above and the log on PS5ISO). Linux keeps running.")
        return 1
    # the password is in Windows now; don't leave it readable on PS5ISO (nor in the work copy)
    if extra.is_file():
        res = blank_password(extra)
        if res == "blanked":
            scr.say("Answer file: password line blanked on PS5ISO (the password is set in Windows).")
        elif res != "none":
            scr.say(f"Answer file: could not blank the password line ({res[8:]}). Delete ps5win-answers.txt "
                    "from PS5ISO on the Mac.")
    for f in (answers, work / "dry/unattend.xml", work / "real/unattend.xml"):
        try:
            f.unlink()
        except OSError:
            pass
    if not Path("/etc/ps5win.conf").is_file():
        scr.say("\nInstalled, but /etc/ps5win.conf is missing: not starting Windows.")
        return 1
    scr.say("\nWindows is installed. Starting it now (the first start finishes Windows Setup by itself).")
    os.execv(str(START), [str(START)])


if __name__ == "__main__":
    sys.exit(main())
