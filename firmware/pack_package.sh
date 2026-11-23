#!/bin/bash
# SPDX-License-Identifier: MIT
# Packs a launch package folder into its deploy.tgz without any build-machine metadata: plain ustar (no pax
# headers, so no macOS extended attributes such as com.apple.provenance / com.apple.macl), owner root:root, members in
# the fixed order the launcher expects. Checks launch.sha256 first and the finished archive afterwards.
# Usage: firmware/pack_package.sh <package folder>     e.g. firmware/pack_package.sh build/boot41
set -euo pipefail
d=${1:?package folder}
cd "$d"
files=(windows.bundle windows.bzImage kms_linear.py run_stage.sh kstub-table.txt launch.sha256)
for f in "${files[@]}"; do [ -f "$f" ] || { echo "missing $f"; exit 1; }; done
shasum -a 256 -c launch.sha256 >/dev/null || { echo "launch.sha256 does not match the folder"; exit 1; }
rm -f deploy.tgz.new
COPYFILE_DISABLE=1 tar --format ustar --no-xattrs --no-mac-metadata --no-acls --no-fflags \
  --uid 0 --gid 0 --uname root --gname root -czf deploy.tgz.new "${files[@]}"
python3 -I - deploy.tgz.new "${files[@]}" <<'EOF'
import hashlib, sys, tarfile
t = tarfile.open(sys.argv[1])
ms = t.getmembers()
assert [m.name for m in ms] == sys.argv[2:], [m.name for m in ms]
for m in ms:
    assert not m.pax_headers, (m.name, m.pax_headers)
    assert (m.uid, m.gid, m.uname, m.gname) == (0, 0, "root", "root"), m.name
    assert hashlib.sha256(t.extractfile(m).read()).digest() == hashlib.sha256(open(m.name, "rb").read()).digest(), m.name
print("deploy.tgz: %d members, no pax/xattr headers, contents match the folder" % len(ms))
EOF
mv deploy.tgz.new deploy.tgz
shasum -a 256 deploy.tgz
