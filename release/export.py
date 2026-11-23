#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copies the files named by release/export-allowlist.txt into a fresh folder (the public repo tree).
# Usage: release/export.py <empty or new output folder>   (default: build/public-export)
# Prints every exported file; refuses a non-empty target so nothing stale survives. Symlinks are skipped (listed).
import re, shutil, sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
out = Path(sys.argv[1] if len(sys.argv) > 1 else root / "build" / "public-export").resolve()


def to_regex(pat):
    if pat.endswith("/"):
        pat += "**"
    rx = ""
    i = 0
    while i < len(pat):
        if pat.startswith("**/", i):
            rx += "(?:.*/)?"; i += 3
        elif pat.startswith("**", i):
            rx += ".*"; i += 2
        elif pat[i] == "*":
            rx += "[^/]*"; i += 1
        elif pat[i] == "?":
            rx += "[^/]"; i += 1
        else:
            rx += re.escape(pat[i]); i += 1
    return re.compile(rx + r"\Z")


inc, exc, extra = [], [], []
for line in (root / "release" / "export-allowlist.txt").read_text().splitlines():
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    if " => " in line:                                   # "source => published path": an extra copy under another name
        src, dst = (x.strip() for x in line.split(" => "))
        extra.append((src, dst))
        continue
    (exc if line.startswith("!") else inc).append((line.lstrip("!"), to_regex(line.lstrip("!"))))

if out.exists() and any(out.iterdir()):
    sys.exit(f"refusing: {out} is not empty")
if root in out.parents and "build" not in out.relative_to(root).parts[:1]:
    sys.exit("refusing: output inside the repo must be under build/")

files, links, used = [], [], set()
for p in sorted(root.rglob("*")):
    rel = p.relative_to(root).as_posix()
    if rel.startswith("build/") or rel.startswith(".git/"):
        continue
    hit = [pat for pat, rx in inc if rx.match(rel)]
    if not hit or any(rx.match(rel) for _, rx in exc):
        continue
    if p.is_symlink():
        links.append(rel); used.update(hit); continue
    if p.is_file():
        files.append(rel); used.update(hit)

for rel in files:
    dst = out / rel
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / rel, dst)
    print(rel)
for src, dst in extra:
    if not (root / src).is_file():
        sys.exit(f"allowlist copy source missing: {src}")
    (out / dst).parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / src, out / dst)
    print(dst)
for rel in links:
    print(f"skipped symlink: {rel}", file=sys.stderr)
for pat, _ in inc:
    if pat not in used:
        print(f"allowlist line matched nothing: {pat}", file=sys.stderr)
print(f"{len(files)} files -> {out}", file=sys.stderr)
