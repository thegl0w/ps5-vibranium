#!/usr/bin/env python3
"""Proves the one-pass apply on the Mac: in-memory overlay on a read-only WIM, then extraction sees our files.

Usage: overlay_test.py <install.wim> <index> <hive dir from offline_reg.py> <scratch dir>
 1. synthetic: capture a tiny WIM, overlay (add, replace, delete), extract the WHOLE image, compare.
 2. real: open install.wim read-only, overlay the edited SYSTEM/SOFTWARE hives + a C:\\ps5 tree + Panther\\unattend.xml
    + Setup\\Scripts\\SetupComplete.cmd, extract only those paths (+ one untouched file), compare byte for byte, and
    check that the WIM file itself did not change (size, mtime, SHA-256 of header and tail).
"""
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from wimapi import Wim, EXTRACT_FLAG_NO_ACLS, EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE  # noqa: E402


def fingerprint(p):
    st = os.stat(p)
    with open(p, "rb") as f:
        head = f.read(1 << 20)
        f.seek(max(0, st.st_size - (1 << 20)))
        tail = f.read()
    return st.st_size, st.st_mtime_ns, hashlib.sha256(head + tail).hexdigest()


def synthetic(scratch):
    src, ov, out = scratch / "syn-src", scratch / "syn-ov", scratch / "syn-out"
    for d in (src, ov, out):
        shutil.rmtree(d, ignore_errors=True)
    (src / "Windows/System32/config").mkdir(parents=True)
    (src / "Windows/System32/config/SYSTEM").write_bytes(b"original hive")
    (src / "Windows/keep.txt").write_text("untouched")
    (src / "Windows/remove.me").write_text("to be deleted")
    wimf = scratch / "syn.wim"
    wimf.unlink(missing_ok=True)
    subprocess.run(["wimcapture", str(src), str(wimf), "syn", "--quiet"], check=True, stdout=subprocess.DEVNULL)
    before = fingerprint(wimf)
    (ov / "ps5").mkdir(parents=True)
    (ov / "ps5/marker.txt").write_text("ours")
    (ov / "SYSTEM").write_bytes(b"edited hive")
    with Wim(wimf) as w:
        w.overlay(1, adds=[(ov / "ps5", "/ps5"), (ov / "SYSTEM", "/Windows/System32/config/SYSTEM")],
                  deletes=["/Windows/remove.me"])
        seen = []
        w.extract_image(1, out, EXTRACT_FLAG_NO_ACLS, progress=lambda d, n: seen.append((d, n)))
    assert seen and seen[-1][0] == seen[-1][1] > 0, seen
    assert (out / "Windows/System32/config/SYSTEM").read_bytes() == b"edited hive"
    assert (out / "ps5/marker.txt").read_text() == "ours"
    assert (out / "Windows/keep.txt").read_text() == "untouched"
    assert not (out / "Windows/remove.me").exists()
    assert fingerprint(wimf) == before, "synthetic WIM changed on disk"
    print("synthetic: whole-image extract with add/replace/delete overlay OK, progress reported; WIM unchanged")


def real(wim, index, hives, scratch):
    here = Path(__file__).resolve().parents[1]
    ov, out = scratch / "real-ov", scratch / "real-out"
    for d in (ov, out):
        shutil.rmtree(d, ignore_errors=True)
    payload = os.environ.get("PAYLOAD")          # a tree from build_payload.py (OUTDIR/ps5), else a placeholder
    if payload:
        shutil.copytree(payload, ov / "ps5")
    else:
        (ov / "ps5/gpu").mkdir(parents=True)
        (ov / "ps5/gpu/README.txt").write_text("placeholder for the PS5 payload tree")
    before = fingerprint(wim)
    unattend = Path(os.environ["UNATTEND"]) if os.environ.get("UNATTEND") else here.parent / "winpe/unattend.xml"
    setupc = ov / "ps5/setup/SetupComplete.cmd" if payload else here.parent / "winpe/SetupComplete.cmd"
    adds = [(hives / "SYSTEM", "/Windows/System32/config/SYSTEM"),
            (hives / "SOFTWARE", "/Windows/System32/config/SOFTWARE"),
            (ov / "ps5", "/ps5"),
            (unattend, "/Windows/Panther/unattend.xml"),
            (setupc, "/Windows/Setup/Scripts/SetupComplete.cmd")]
    checks = {"/Windows/System32/config/SYSTEM": hives / "SYSTEM",
              "/Windows/System32/config/SOFTWARE": hives / "SOFTWARE",
              **({"/ps5/gpu/wddm/ps5wddm.sys": ov / "ps5/gpu/wddm/ps5wddm.sys",
                  "/ps5/setup/ps5-setup.cmd": ov / "ps5/setup/ps5-setup.cmd",
                  "/ps5/gpu/d3d10/libgallium_d3d10.dll": ov / "ps5/gpu/d3d10/libgallium_d3d10.dll"} if payload else
                 {"/ps5/gpu/README.txt": ov / "ps5/gpu/README.txt"}),
              "/Windows/Panther/unattend.xml": unattend,
              "/Windows/Setup/Scripts/SetupComplete.cmd": setupc}
    with Wim(wim) as w:
        w.overlay(int(index), adds=adds)
        out.mkdir()
        w.extract_paths(int(index), out, list(checks) + ["/Windows/System32/ntoskrnl.exe"],
                        EXTRACT_FLAG_NO_ACLS | EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE)
    for path, want in checks.items():
        got = out / Path(path).name
        assert got.read_bytes() == want.read_bytes(), f"{path} differs"
    assert (out / "ntoskrnl.exe").stat().st_size > 5_000_000
    after = fingerprint(wim)
    assert after == before, f"install.wim changed on disk: {before} -> {after}"
    print(f"real: {len(checks)} overlaid paths read back byte for byte from image {index}; untouched ntoskrnl.exe "
          f"extracted; install.wim unchanged (size {after[0]:,}, mtime, head/tail hash)")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    wim, index, hives, scratch = sys.argv[1], sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4])
    scratch.mkdir(parents=True, exist_ok=True)
    synthetic(scratch)
    real(wim, index, hives, scratch)
