#!/usr/bin/env python3
"""Assemble /opt/ps5win/installer/ for the Toshiba installer image (windows-handoff/image/, the image session's).

Usage: make_image_tree.py OUTDIR [--package build/boot48] [--frozen PAYLOAD.json] [--clean]
  OUTDIR/installer/   autorun (entry point), autorun.py, install.py and the tools it calls, lib/, deny.txt,
                      kstub-table.txt (from the launch package the image ships, so the kernel check and the firmware
                      agree), payload/ (build_payload.py: the C:\\ps5 tree, hash-checked), TREE.txt (what went in)
  Hand OUTDIR/installer to the image build (assemble-inside.sh copies /in/installer to /opt/ps5win/installer).
Not included: tests, docs, fixtures, proposals, the BCD reference store, anything personal.
"""
import argparse
import hashlib
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
WH = HERE.parent
# Firmware bundles that must never ship. Every bundle before the 2026-11-22 fix skips TraceInstall (EFER bit 16,
# reset/shutdown, the ExitBootServices hooks: HAL patch + kernel stub) once the 9 trace files exist on PS5BOOT, so from
# the 10th start on Windows hangs at a plain logo (GPU session's finding; firmware/windows_bds.inc + windows_trace.inc).
BAD_BUNDLES = {
    "2fe93ce39f60956635e2fe0472fa92a7f162128d492fb0b7eff00d5715e9c85f": "boot33/boot34: TraceInstall skipped after 9 boots",
    "e6154dd8a29f628be66e6cddf1f6b546e5d0a8381e29e716f6cf46c3cc65ef9f": "boot35-boot37: TraceInstall skipped after 9 boots",
    "f11fb9392943475b": "first trace-fix build, superseded by a3e9544b (BootMark best effort)",   # prefix match
}
FILES = ["ps5win-answers.example.txt", "autorun", "autorun.py", "install.py", "find_iso.py", "plan_layout.py", "check_kernel.py", "offline_reg.py",
         "make_unattend.py", "make_bcd.py", "lib/udf.py", "lib/wim.py", "lib/wimapi.py", "lib/hive.py", "lib/bcd.py",
         "lib/writer.py", "lib/kpattern.py"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir")
    ap.add_argument("--package", default=str(WH / "build/boot48"))
    ap.add_argument("--frozen")
    ap.add_argument("--clean", action="store_true")
    a = ap.parse_args()
    out = Path(a.outdir).resolve()
    tree = out / "installer"
    if HERE in out.parents or out == HERE:
        sys.exit("OUTDIR must not be inside installer/")
    if tree.exists():
        if not (a.clean and (tree / "TREE.txt").exists()):
            sys.exit(f"{tree} exists (use --clean on a previous tree)")
        shutil.rmtree(tree)
    table = Path(a.package) / "kstub-table.txt"
    if not table.is_file():
        sys.exit(f"no {table}: build the launch package first")
    bundle = Path(a.package) / "windows.bundle"
    bundle_sha = hashlib.sha256(bundle.read_bytes()).hexdigest() if bundle.is_file() else ""
    bad = next((why for h, why in BAD_BUNDLES.items() if bundle_sha.startswith(h)), None)
    if bad:
        sys.exit(f"{Path(a.package).name}: firmware bundle {bundle_sha[:16]}… must not ship ({bad})")
    # the bundle that goes out also travels in the package's deploy.tgz: check they agree
    import tarfile
    with tarfile.open(Path(a.package) / "deploy.tgz") as tf:
        inner = tf.extractfile("windows.bundle").read()
        # no build-machine metadata (macOS tar writes com.apple.* extended attributes as pax headers): pack with
        # firmware/pack_package.sh
        meta = [m.name for m in tf.getmembers() if m.pax_headers or m.uname not in ("", "root")]
    if meta:
        sys.exit(f"{Path(a.package).name}: deploy.tgz carries build-machine metadata (pax headers / owner) in {meta}; "
                 "repack with firmware/pack_package.sh")
    if hashlib.sha256(inner).hexdigest() != bundle_sha:
        sys.exit(f"{Path(a.package).name}: deploy.tgz carries a different windows.bundle than the package folder")
    (tree / "lib").mkdir(parents=True)
    for f in FILES:
        shutil.copy2(HERE / f, tree / f)
    for f in ("autorun", "autorun.py", "install.py"):
        (tree / f).chmod(0o755)
    shutil.copy2(HERE / "deny.txt", tree / "deny.txt")       # release list: no USB IDs (no user's enclosure blocked)
    shutil.copy2(table, tree / "kstub-table.txt")
    cmd = [sys.executable, "-I", str(HERE / "build_payload.py"), str(tree / "payload")]
    if a.frozen:
        cmd += ["--frozen", a.frozen]
    p = subprocess.run(cmd, capture_output=True, text=True)
    print(p.stdout.strip())
    if p.returncode != 0:
        shutil.rmtree(tree)
        sys.exit("payload failed: no tree written")
    sys.path.insert(0, str(HERE))
    import build_payload
    needles = build_payload.private_needles()
    hits = sorted({f"{f.relative_to(tree)}: {lab}" for f in tree.rglob("*") if f.is_file()
                   for lab, n in needles if n in f.read_bytes()})
    if hits:
        shutil.rmtree(tree)
        sys.exit("private details in the tree (values not shown):\n  " + "\n  ".join(hits))
    lines = [f"PS5 Windows installer tree, made {time.strftime('%Y-%m-%d %H:%M:%S')}",
             f"launch package: {Path(a.package).name} (kstub-table.txt copied from it; firmware bundle {bundle_sha})"]
    for f in sorted(tree.rglob("*")):
        if f.is_file() and "payload" not in f.relative_to(tree).parts:
            lines.append(f"{hashlib.sha256(f.read_bytes()).hexdigest()}  {f.relative_to(tree)}")
    lines.append("payload: see payload/payload.json and payload/ps5/setup/MANIFEST.sha256")
    (tree / "TREE.txt").write_text("\n".join(lines) + "\n")
    size = sum(f.stat().st_size for f in tree.rglob("*") if f.is_file())
    print(f"tree OK: {tree} ({size / 2**20:.0f} MiB)")


if __name__ == "__main__":
    main()
