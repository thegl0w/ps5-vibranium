#!/bin/bash
# Offline tests for the PS5 Windows installer scripts (Mac or Linux). Nothing here touches a disk: layout tests use
# JSON fixtures; ISO tests read the user's ISO in place (read-only); hive/overlay tests write only into $SCRATCH.
# Usage: tests/run_tests.sh [ISO or ISO folder]   (ISO tests are skipped without one)
#        SCRATCH=/some/dir to choose where hive copies go (default: a new mktemp dir)
set -u
cd "$(dirname "$0")/.."
F=tests/fixtures
PY="python3 -I"
pass=0; fail=0
check() {   # check <name> <expected exit> <grep pattern or -> -- command...
  local name=$1 want=$2 pat=$3; shift 4
  out=$("$@" 2>&1); got=$?
  if [ "$got" = "$want" ] && { [ "$pat" = - ] || grep -q -- "$pat" <<<"$out"; }; then
    pass=$((pass+1)); echo "ok   $name"
  else
    fail=$((fail+1)); echo "FAIL $name (exit $got, want $want, pattern '$pat')"; sed 's/^/     | /' <<<"$out" | head -20
  fi
}

echo "== layout planner (fixtures)"
check "erase empty USB target"            1 "ERASE 0002"           -- $PY plan_layout.py --fixture $F/two-disk.json --target-serial T7TARGET0002 --mode erase
check "answer file confirms"              0 "CONFIRMED"            -- $PY plan_layout.py --fixture $F/two-disk.json --target-serial T7TARGET0002 --mode erase --answer-file $F/answer-ok.txt
check "wrong answer file does not"        1 "stops here"           -- $PY plan_layout.py --fixture $F/two-disk.json --target-serial T7TARGET0002 --mode erase --answer-file $F/answer-wrong.txt
check "internal NVMe refused"             4 "internal SSD"         -- $PY plan_layout.py --fixture $F/two-disk.json --target-serial SONYINTERNAL --mode erase
check "internal SSD by PCI path refused"  4 "0000:40:00.0"         -- $PY plan_layout.py --fixture $F/two-disk.json --target-serial DISGUISED --mode erase
check "Linux disk erase refused"          4 "Linux is running"     -- $PY plan_layout.py --fixture $F/two-disk.json --target-serial LNXSSD0001 --mode erase
check "ISO disk erase refused"            4 "holds the Windows ISO" -- $PY plan_layout.py --fixture $F/two-disk.json --target-serial ISOSTICK0003 --mode erase --iso-dev /dev/sdc1
check "MBR disk append refused"           4 "no GPT"               -- $PY plan_layout.py --fixture $F/two-disk.json --target-serial ISOSTICK0003 --mode append
check "unknown serial refused"            4 "0 disks have serial"  -- $PY plan_layout.py --fixture $F/two-disk.json --target-serial NOPE --mode erase
check "single-disk append"                1 "kept   /dev/sda2"     -- $PY plan_layout.py --fixture $F/single-disk.json --target-serial ONEDISK0004 --mode append
check "append keeps partition numbers"    1 "NEW    #3"            -- $PY plan_layout.py --fixture $F/single-disk.json --target-serial ONEDISK0004 --mode append
check "append size cap"                   1 "below the recommended" -- $PY plan_layout.py --fixture $F/single-disk.json --target-serial ONEDISK0004 --mode append --windows-gib 48
check "too small refused"                 4 "largest free space"   -- $PY plan_layout.py --fixture $F/small-disk.json --target-serial SMALL0005 --mode append
check "second install refused"            4 "reinstall"            -- $PY plan_layout.py --fixture $F/installed-disk.json --target-serial AGAIN0006 --mode append
check "plan: no overlap, PS5BOOT is an ESP" 0 -                      -- $PY -c '
import json,subprocess,sys
o=json.loads(subprocess.run([sys.executable,"-I","plan_layout.py","--fixture","tests/fixtures/single-disk.json","--target-serial","ONEDISK0004","--mode","append","--json"],capture_output=True,text=True).stdout)
d=json.load(open("tests/fixtures/single-disk.json"))["disks"][0]
spans=[(p["start"]*512,(p["start"]+p["sectors"])*512) for p in d["partitions"]]+[(p["start"],p["start"]+p["size"]) for p in o["plan"]["new"]]
spans.sort()
assert all(a[1]<=b[0] for a,b in zip(spans,spans[1:])), spans
assert spans[-1][1] <= (d["size"]//512-34)*512
assert all(p["start"]%(1<<20)==0 for p in o["plan"]["new"])
b, w = o["plan"]["new"]
assert b["label"] == "PS5BOOT" and b["type"] == "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", b
assert w["label"] == "PS5WIN" and w["type"] == "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", w
assert any(c.startswith("sgdisk -n") and "-t 3:C12A7328-F81F-11D2-BA4B-00A0C93EC93B" in c and "-c 3:PS5BOOT" in c for c in o["would_run"])'

echo "== block-device scan filter (lsblk fixture)"
check "scan skips NVMe, target, swap"     0 -                      -- $PY -c '
import json,sys; sys.path.insert(0,".")
import find_iso
r=find_iso.block_device_roots(json.load(open("tests/fixtures/lsblk.json")),{"T7TARGET0002"})
devs=[x["dev"] for x in r]
assert devs==["/dev/sda1","/dev/sda2","/dev/sdc1"], devs
assert all("nvme" not in d for d in devs)'

echo "== unattend.xml generator"
U=$(mktemp -d)
ln -s /usr/share/zoneinfo/Europe/Berlin "$U/lt-berlin"; ln -s /usr/share/zoneinfo/Antarctica/Troll "$U/lt-odd"; : > "$U/lt-none"
check "defaults: zone from the Linux link"  0 "W. Europe Standard Time" -- $PY make_unattend.py --out "$U/a.xml" --localtime "$U/lt-berlin"
check "unknown zone falls back to UTC"     0 "time_zone: UTC"        -- $PY make_unattend.py --out "$U/b.xml" --localtime "$U/lt-odd"
check "no zone link falls back to UTC"     0 "time_zone: UTC"        -- $PY make_unattend.py --out "$U/c.xml" --localtime "$U/lt-none"
check "answers used, secrets not printed"  0 "password: set"         -- $PY make_unattend.py --out "$U/d.xml" --answers $F/answers-full.txt --localtime "$U/lt-none"
check "too-long computer name refused"     4 "computer_name"         -- $PY make_unattend.py --out "$U/e.xml" --answers $F/answers-badname.txt --localtime "$U/lt-none"
check "built-in account name refused"      4 "user_name"             -- $PY make_unattend.py --out "$U/f.xml" --answers $F/answers-baduser.txt --localtime "$U/lt-none"
check "malformed product key refused"      4 "product_key"           -- $PY make_unattend.py --out "$U/g.xml" --answers $F/answers-badkey.txt --localtime "$U/lt-none"
check "answers example: inert as shipped"   0 -                       -- $PY -c '
import sys; sys.path.insert(0, "."); import install
assert install.read_kv("ps5win-answers.example.txt") == {}'
sed -n "s/^#\(computer_name\|user_name\|password\|time_zone\|locale\|windows_gib\)=/\1=/p" ps5win-answers.example.txt > "$U/answers-uncommented.txt"
check "answers example: every value valid"   0 "time_zone: UTC"       -- $PY make_unattend.py --out "$U/ex.xml" --answers "$U/answers-uncommented.txt" --localtime "$U/lt-none"
sed -n "s/^#\(product_key\)=/\1=/p" ps5win-answers.example.txt > "$U/answers-pk.txt"
check "answers example: placeholder key refused" 4 "example placeholder" -- $PY make_unattend.py --out "$U/ex2.xml" --answers "$U/answers-pk.txt" --localtime "$U/lt-none"
check "answers example: keys documented = keys used" 0 -             -- $PY -c '
import re; from pathlib import Path
doc = set(re.findall(r"(?m)^#(\w+)=", Path("ps5win-answers.example.txt").read_text()))
used = {"computer_name", "user_name", "password", "time_zone", "locale", "product_key", "windows_gib", "experimental", "edition"}
assert doc == used, (doc, used)
assert len(open("ps5win-answers.example.txt").read().split("\n")) < 60'
check "XML content (defaults and answers)" 0 -                       -- $PY -c '
import sys, subprocess, base64, xml.etree.ElementTree as ET
d = sys.argv[1]; ns = {"u": "urn:schemas-microsoft-com:unattend"}
a = ET.parse(d + "/a.xml").getroot()
assert a.find(".//u:ComputerName", ns).text == "PS5WIN" and a.find(".//u:Username", ns).text == "ps5"
assert a.find(".//u:LocalAccount/u:Password/u:PlainText", ns).text == "true" and not a.find(".//u:LocalAccount/u:Password/u:Value", ns).text
assert a.find(".//u:ProductKey", ns) is None
cmds = [(c.find("u:Order", ns).text, c.find("u:CommandLine", ns).text) for c in a.findall(".//u:FirstLogonCommands/u:SynchronousCommand", ns)]
assert cmds == [("1", r"cmd /c C:\ps5\setup\ps5-setup.cmd"), ("2", r"cmd /c C:\ps5\firstlogon.cmd")], cmds
raw = open(d + "/d.xml", "rb").read()
assert b"Fixture-Pass-123" not in raw
x = ET.fromstring(raw)
v = x.find(".//u:AutoLogon/u:Password/u:Value", ns).text
assert base64.b64decode(v).decode("utf-16-le") == "Fixture-Pass-123Password"
assert x.find(".//u:ProductKey", ns).text == "ABCDE-FGHIJ-KLMNO-PQRST-UVWXY"
assert x.find(".//u:TimeZone", ns).text == "Tokyo Standard Time" and x.find(".//u:UILanguage", ns).text == "en-GB"
out = subprocess.run([sys.executable, "-I", "make_unattend.py", "--out", d + "/z.xml", "--answers", "tests/fixtures/answers-full.txt", "--localtime", d + "/lt-none"], capture_output=True, text=True).stdout
assert "Fixture-Pass-123" not in out and "ABCDE" not in out.upper().replace("PRODUCT_KEY", "")' "$U"

echo "== release hygiene (this folder)"
check "installer/ holds no private details"  0 -                     -- $PY -c '
import sys; sys.path.insert(0, "."); import build_payload as b
from pathlib import Path
n = b.private_needles(); hits = set()
for f in Path(".").rglob("*"):
    if f.is_file() and "__pycache__" not in f.parts:
        d = f.read_bytes(); low = d.lower()
        hits |= {f"{f}: {lab}" for lab, x in n if x in d or (x.isascii() and x.lower() in low)}
assert not hits, sorted(hits)'

check "release wording: no console-hacking terms"   0 -             -- $PY -c '
import re; from pathlib import Path
hits = [str(f) for f in Path(".").rglob("*") if f.is_file() and "__pycache__" not in f.parts
        and re.search(rb"(?i)jail.?break|" + b"expl" + b"oit", f.read_bytes())]
assert not hits, hits'
check "first logon: watchdog started, GPU autostart not" 0 -                  -- $PY -c '
from pathlib import Path
s = Path("payload/setup/ps5-setup.cmd").read_text()
c, r = s.index("schtasks /create /f /tn ps5-guard"), s.index("schtasks /run /tn ps5-guard")
assert c < r
assert "schtasks /run /tn ps5-gpu-autostart" not in s   # not in the first session (r13: suspect; READ ME: restart)'
check "READ ME FIRST: five points, URL placeholder" 0 -               -- $PY -c '
from pathlib import Path
t = Path("payload/setup/readme-first.txt").read_text()
for s in ("Restart one more time now", "Microsoft Basic Display Adapter", "Windows Update is turned off on purpose",
          "Set a password", "Sign-in options", "EasyAntiCheat, BattlEye, Vanguard", "<project repository URL>"):
    assert s in t, s
f = Path("payload/setup/firstlogon.cmd").read_bytes()
assert b"READ ME FIRST.txt" in f and b"readme-first.txt" in f'

echo "== write path (simulated system)"
check "writer: order, re-checks, stops"     0 "10 writer scenarios passed" -- $PY tests/writer_test.py
check "answer file: password blanked"     0 "10 answer-file cases passed" -- $PY tests/answers_test.py
check "editions + pattern stub checks"    0 "edition/pattern cases passed" -- $PY tests/edition_test.py
check "VRAM carve-out from the package"    0 "6 carve cases passed" -- $PY tests/carve_test.py

echo "== payload (C:\\ps5 tree from this Mac's build outputs)"
if [ -f ../build/ps5gpu/wddm/ps5wddm.sys ]; then
  P=$(mktemp -d)
  # negative manifests derived from the live one, so they never drift from it
  M=payload/manifest.txt
  sed 's#/p5eth.cat  #/p5eth-missing.cat #' $M > "$P/manifest-missing.txt"
  sed 's#dxvk-3.1.1/x64/d3d11.dll#dxvk-3.1.1/x32/d3d11.dll#' $M > "$P/manifest-wrongarch.txt"
  sed 's#^\(eth.p5eth.sys *| *\)[^ ]*#\1build/cleanroom-p5eth/p5eth-unsigned.sys#' $M > "$P/manifest-unsigned.txt"
  grep -v 'gpu.d3d10.libgallium_d3d10.dll' $M > "$P/manifest-noumd.txt"
  { cat $M; echo 'wifi\p5net.sys | build/ps5wifi/p5net.sys | driver'; } > "$P/manifest-wifi.txt"
  python3 -c 'import sys; open(sys.argv[1],"wb").write(b"\x30\x82\x04\x00" + bytes(1100))' "$P/foreign.cat"
  sed "s#^\(eth.p5eth.cat *| *\)[^ ]*#\1$P/foreign.cat#" $M > "$P/manifest-foreigncat.txt"
  printf -- '-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n' > "$P/testkey.pem"
  { cat $M; echo "gpu\\wddm\\key.pem | $P/testkey.pem | file"; } > "$P/manifest-key.txt"
  MDEV="$P/manifest-devcert.txt"; sed -e "s#build/release-signed/wddm/#build/ps5gpu/wddm-boot/#" -e "s#build/release-signed/eth/#build/cleanroom-p5eth/#" -e "s#build/release-signed/gpu/#build/ps5gpu/#" $M > "$MDEV"   # dev-cert wddm files
  sed "s#^\(gpu.wddm.ps5wddm.cat *| *\)[^ ]*#\1build/pdbfix-backup/ps5wddm.cat#" "$MDEV" > "$P/manifest-stalecat.txt"
  # a catalog with two equal-length members swapped (same bytes, wrong order) must be refused as unsorted
  python3 -c '
import sys; sys.path.insert(0, "lib"); import catalog
d = bytearray(open("../build/ps5gpu/wddm/ps5wddm.cat", "rb").read())
m = [t for t, _ in catalog.members(bytes(d))]
i, j = next((a, b) for a in range(len(m)) for b in range(a + 1, len(m)) if len(m[a]) == len(m[b]))
pa, pb = bytes(d).index(m[i]), bytes(d).index(m[j])
d[pa:pa + len(m[i])], d[pb:pb + len(m[j])] = m[j], m[i]
assert catalog.unsorted(bytes(d)), "swap did not unsort"
open(sys.argv[1], "wb").write(bytes(d))' "$P/unsorted.cat"
  sed "s#^\(gpu.wddm.ps5wddm.cat *| *\)[^ ]*#\1$P/unsorted.cat#" "$MDEV" > "$P/manifest-unsorted.txt"
  python3 -c 'import getpass,sys; open(sys.argv[1],"w").write("C:\\Users\\" + getpass.getuser() + "\\x.pdb\n")' "$P/userpath.txt"
  { cat $M; echo "gpu\\userpath.txt | $P/userpath.txt | file"; } > "$P/manifest-user.txt"
  check "payload builds and checks"        0 "payload OK"            -- $PY build_payload.py "$P/ok"
  check "missing catalog refused"          1 "source missing"        -- $PY build_payload.py "$P/m1" --manifest "$P/manifest-missing.txt"
  check "32-bit DLL in 64-bit folder"      1 "want 0x8664"           -- $PY build_payload.py "$P/m2" --manifest "$P/manifest-wrongarch.txt"
  check "unsigned driver refused"          1 "test/unsigned"         -- $PY build_payload.py "$P/m3" --manifest "$P/manifest-unsigned.txt"
  check "INF's user-mode driver required"  1 "user-mode driver"      -- $PY build_payload.py "$P/m4" --manifest "$P/manifest-noumd.txt"
  if [ -f ../build/pdbfix-backup/ps5wddm.cat ]; then
    check "catalog without the driver's digests refused" 1 "SHA1 digest" -- $PY build_payload.py "$P/m9" --manifest "$P/manifest-stalecat.txt"
  fi
  check "catalog members out of order refused" 1 "not in ascending order" -- $PY build_payload.py "$P/m11" --manifest "$P/manifest-unsorted.txt"
  check "shipped catalogs: 4 exact members, all sorted" 0 -  -- $PY -c '
import sys, hashlib; sys.path[:0] = [".", "lib"]; import catalog, build_payload as b
cats = [s for d, s, k in b.read_manifest() if k == "signed-cat"]
assert len(cats) == 2, cats
for c in cats:
    d = open("../" + c, "rb").read(); m = catalog.members(d)
    assert len(m) == 4 and sorted(len(t) for t, _ in m) == [20, 20, 32, 32], (c, len(m))
    assert not catalog.unsorted(d), c
assert len(b.UNSORTED_CAT_EXCEPTIONS) == 0'
  check "build machine user name refused, not shown" 0 -           -- $PY -c '
import subprocess, sys, getpass
o = subprocess.run([sys.executable, "-I", "build_payload.py", sys.argv[1], "--manifest", sys.argv[2]], capture_output=True, text=True)
assert o.returncode == 1 and "build machine user name" in o.stdout, o.stdout
assert getpass.getuser() not in o.stdout + o.stderr' "$P/m10" "$P/manifest-user.txt"
  check "private key never shipped"         1 "private key"          -- $PY build_payload.py "$P/m8" --manifest "$P/manifest-key.txt"
  check "catalog signed by another cert refused" 1 "not signed with the shipped certificate" -- $PY build_payload.py "$P/m7" --manifest "$P/manifest-foreigncat.txt"
  check "Wi-Fi driver with network details refused" 1 "private network details" -- $PY build_payload.py "$P/m5" --manifest "$P/manifest-wifi.txt"
  check "privacy failure shows no values"   0 -                     -- $PY -c '
import subprocess, sys
o = subprocess.run([sys.executable, "-I", "build_payload.py", sys.argv[1], "--manifest", sys.argv[2]], capture_output=True, text=True).stdout
lines = [l for l in o.splitlines() if l.startswith("  ")]
assert lines and all(l.strip().startswith("wifi\\p5net.sys: ") and l.split(": ", 1)[1].split(" #")[0] in ("network name", "access point MAC", "console Wi-Fi MAC") for l in lines), o' "$P/m6" "$P/manifest-wifi.txt"
  check "frozen payload: same sources pass" 0 "matches the frozen"    -- $PY build_payload.py "$P/f1" --frozen "$P/ok/payload.json"
  python3 -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); d["sha256"]["gpu\\vk\\vulkan_radeon.dll"]="0"*64; json.dump(d,open(p,"w"))' "$P/f1/payload.json" 2>/dev/null
  check "frozen payload: a changed file fails" 1 "gpu.vk.vulkan_radeon.dll" -- $PY build_payload.py "$P/f2" --frozen "$P/f1/payload.json"
  check "no output inside installer/"      1 "not be inside installer" -- $PY build_payload.py tests/out-here
  check "staged tree: CRLF, task XML, hashes" 0 -                    -- $PY -c '
import sys, hashlib, xml.etree.ElementTree as ET
from pathlib import Path
r = Path(sys.argv[1]) / "ok/ps5"
for f in list(r.rglob("*.cmd")) + list(r.rglob("*.ps1")):
    b = f.read_bytes(); assert b.count(b"\n") == b.count(b"\r\n"), f
ns = {"t": "http://schemas.microsoft.com/windows/2004/02/mit/task"}
for x in (r / "setup/tasks").glob("*.xml"):
    b = x.read_bytes(); assert b[:2] == b"\xff\xfe", x
    t = ET.fromstring(b.decode("utf-16-le").lstrip("\ufeff").replace("encoding=\"UTF-16\"", ""))
    assert t.find("t:Principals/t:Principal/t:UserId", ns).text == "S-1-5-18"
    assert t.find("t:Triggers/t:BootTrigger", ns) is not None
guard = (r / "setup/tasks/ps5-guard.xml").read_bytes().decode("utf-16-le")
assert "<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>" in guard
for line in (r / "setup/MANIFEST.sha256").read_text().splitlines():
    h, p = line.split("  ", 1)
    assert hashlib.sha256((r / p.replace("\\", "/")).read_bytes()).hexdigest() == h, p' "$P"
  rm -rf "$P"
else
  echo "skip payload tests (no build outputs here)"
fi

ISO=${1:-}
if [ -n "$ISO" ]; then
  echo "== media (read-only on $ISO)"
  check "find_iso picks IoT LTSC image 2"  0 "verdict: OK, image 2" -- $PY find_iso.py "$ISO"
  if [ -d "$ISO" ]; then WIM="$ISO/sources/install.wim"; else WIM=""; fi
  if [ -n "$WIM" ] && [ -f "$WIM" ]; then
    S=${SCRATCH:-$(mktemp -d)}
    check "kernel stub exists for the ISO kernel" 0 "MATCH windows_kstub-19041.1288.inc" -- $PY check_kernel.py "$WIM" 2
    check "offline registry edits on copies"      0 "values set and verified" -- $PY offline_reg.py "$WIM" 2 "$S/hives" --cert ../build/ps5gpu/wddm/ps5test.cer
    check "test cert trusted offline (Root+TrustedPublisher)" 0 -        -- $PY -c '
import sys, struct, hashlib; sys.path.insert(0, "lib"); from hive import Hive
der = open(sys.argv[2], "rb").read(); th = hashlib.sha1(der).hexdigest().upper()
with Hive(sys.argv[1]) as h:
    for store in ("ROOT", "TrustedPublisher"):
        t, blob = h.get("Microsoft\\SystemCertificates\\" + store + "\\Certificates\\" + th, "Blob")
        assert t == 3, t
        o, props = 0, {}
        while o + 12 <= len(blob):
            pid, one, ln = struct.unpack_from("<III", blob, o); assert one == 1; props[pid] = blob[o + 12:o + 12 + ln]; o += 12 + ln
        assert o == len(blob) and props[32] == der and props[3] == hashlib.sha1(der).digest(), sorted(props)
    assert h.get("Microsoft\\SystemCertificates\\ROOT\\Certificates\\0119E81BE9A14CD8E22F40AC118C687ECBA3F4D8", "Blob")' "$S/hives/SOFTWARE" ../build/ps5gpu/wddm/ps5test.cer
    check "Delivery Optimization off, no invented services" 0 -          -- $PY -c '
import sys, shutil; sys.path[:0] = [".", "lib"]; from hive import Hive, REG_DWORD
import offline_reg
with Hive(sys.argv[1] + "/SYSTEM") as h:
    assert h.get(r"ControlSet001\Services\DoSvc", "Start") == (REG_DWORD, 4)
with Hive(sys.argv[1] + "/SOFTWARE") as h:
    assert h.get(r"Policies\Microsoft\Windows\DeliveryOptimization", "DODownloadMode") == (REG_DWORD, 0)
cp = sys.argv[1] + "/SYSTEM.skiptest"; shutil.copyfile(sys.argv[1] + "/SYSTEM", cp)
offline_reg.apply(cp, [(r"ControlSet001\Services\NoSuchSvc4711", "Start", REG_DWORD, 4)])
with Hive(cp) as h:
    assert not h.node(r"ControlSet001\Services\NoSuchSvc4711")' "$S/hives"
    check "deny list: PCI only, no USB enclosure" 0 -                     -- $PY -c '
import sys; sys.path.insert(0, "lib"); from hive import Hive
k = "Policies\\Microsoft\\Windows\\DeviceInstall\\Restrictions\\DenyDeviceIDs"
with Hive(sys.argv[1]) as h:
    vals = [h.get(k, str(i)) for i in range(1, 10)]
ids = [v[1] for v in vals if v]
assert ids and ids[0].startswith("PCI") and "104D" in ids[0] and all(i.startswith("PCI") for i in ids), ids
assert not any("5910" in i for i in ids), ids' "$S/hives/SOFTWARE"
    if [ -f ../build/boot21/efi-bcd/BCD.ondisk ]; then
      rm -rf "$S/bcdt2"; mkdir -p "$S/bcdt2"
      wimextract "$WIM" 2 /Windows/System32/config/BCD-Template --dest-dir="$S/bcdt2" --no-acls --quiet 2>/dev/null
      SFD=$(ls ../build/wininstall-backup/*.sfdisk 2>/dev/null | head -1)
      check "BCD vs. the Toshiba's bcdboot store"  0 "checker catches"     -- $PY tests/bcd_test.py "$S/bcdt2/BCD-Template" ../build/boot21/efi-bcd/BCD.ondisk "$S/bcdtest" $SFD
    fi
    check "BCD: equal GUIDs refused"              1 "must all differ"       -- $PY make_bcd.py --out "$S/BCD.x" --wim "$WIM" --index 2 --boot-part 41082273-c2c4-4931-a884-0da26096156f --win-part 41082273-c2c4-4931-a884-0da26096156f --disk-guid 4683bec9-76f0-4b8d-83cd-102cd7ca7d2c
    check "zone table vs. the image's own list"   0 "all mapped zones"      -- $PY -c '
import sys; sys.path.insert(0, ".")
import make_unattend as m
zones = m.image_zones(sys.argv[1]); bad = sorted(set(v for v in m.TZ.values() if v not in zones))
assert not bad, bad
print(f"all mapped zones ({len(set(m.TZ.values()))}) are among the image'"'"'s {len(zones)}")' "$S/hives/SOFTWARE"
    check "zone not in the image refused"         4 "not one of the image"  -- $PY make_unattend.py --out "$S/u-bad.xml" --answers $F/answers-badtz.txt --hive "$S/hives/SOFTWARE"
    $PY make_unattend.py --out "$S/unattend-test.xml" --answers $F/answers-full.txt --hive "$S/hives/SOFTWARE" >/dev/null
    check "one-pass overlay apply"                0 "install.wim unchanged" -- env UNATTEND="$S/unattend-test.xml" $PY tests/overlay_test.py "$WIM" 2 "$S/hives" "$S/overlay"
    if [ -f ../build/ps5gpu/wddm/ps5wddm.sys ]; then
      rm -rf "$S/payload-ov"
      $PY build_payload.py "$S/payload-ov" >/dev/null
      check "overlay apply with the real payload" 0 "install.wim unchanged" -- env PAYLOAD="$S/payload-ov/ps5" UNATTEND="$S/unattend-test.xml" $PY tests/overlay_test.py "$WIM" 2 "$S/hives" "$S/overlay"
      # GPU microcode = the image's copy from linux-firmware-amdgpu (zstd-compressed there), unpacked for the test
      FWD="$S/fw"; mkdir -p "$FWD"
      for z in ../image/collect/kernel/firmware/amdgpu/cyan_skillfish2_*.bin.zst; do zstd -dqf "$z" -o "$FWD/$(basename "${z%.zst}")"; done
      cp ../release/licenses/LICENSE.amdgpu "$FWD/"   # AMD's licence travels with the microcode (install.py requires it)
      IA=(--media "$ISO" --fixture $F/two-disk.json --firmware-dir $FWD)
      rm -rf "$S/e2e" "$S/e2e2" "$S/e2e3" "$S/e2e4"
      check "install.py dry run end to end"       0 "DRY RUN COMPLETE" -- $PY install.py --dry-run --answers $F/answers-install.txt "${IA[@]}" --payload "$S/payload-ov" --workdir "$S/e2e"
      check "dry run: staged PS5BOOT + records"   0 -                       -- $PY -c '
import sys, json; from pathlib import Path
w = Path(sys.argv[1]); r = json.loads((w / "report.json").read_text())
assert not any(s["status"] in ("FAIL", "TODO") for s in r["steps"]) and r["steps"][-1]["step"] == "write path" and r["steps"][-1]["status"] == "SKIP"
sys.path.insert(0, "lib"); import bcd
st = bcd.read(str(w / "ps5boot/EFI/Microsoft/Boot/BCD")); assert not bcd.check(st)
bm = st[bcd.BOOTMGR]["elements"]; ld = st[bm[bcd.E["default"]]]["elements"]
pl = json.loads((w / "plan.json").read_text())["plan"]
assert bm[bcd.E["device"]] == {"kind": "gpt-partition", "partition": pl["new"][0]["guid"], "disk": pl["disk_guid"]}
assert ld[bcd.E["device"]] == {"kind": "gpt-partition", "partition": pl["new"][1]["guid"], "disk": pl["disk_guid"]}
assert (w / "ps5boot/EFI/Microsoft/Boot/bootmgfw.efi").read_bytes() == (w / "ps5boot/EFI/Boot/bootx64.efi").read_bytes()
ident = (w / "ps5boot/ps5win.id").read_text(); conf = (w / "ps5win.conf").read_text()
plan = json.loads((w / "plan.json").read_text())["plan"]["new"]
assert f"BOOT_PARTUUID={plan[0]["guid"]}" in conf and f"WIN_PARTUUID={plan[1]["guid"]}" in ident
assert "DISK_SERIAL=T7TARGET0002" in conf' "$S/e2e"
      check "dry run: no target in answers"       1 "needs target_serial"  -- $PY install.py --dry-run --answers $F/answers-notarget.txt "${IA[@]}" --payload "$S/payload-ov" --workdir "$S/e2e2"
      cp -R "$S/payload-ov" "$S/payload-bad"; printf x >> "$S/payload-bad/ps5/gpu/start.cmd"
      check "dry run: tampered payload refused"   1 "bad: gpu"             -- $PY install.py --dry-run --answers $F/answers-install.txt "${IA[@]}" --payload "$S/payload-bad" --workdir "$S/e2e3"
      rm -rf "$S/payload-bad"
      check "dry run: no microcode refused"       1 "missing in"           -- $PY install.py --dry-run --answers $F/answers-install.txt --media "$ISO" --fixture $F/two-disk.json --firmware-dir "$S/hives" --payload "$S/payload-ov" --workdir "$S/e2e4"
      rm -rf "$S/imgtree" "$S/ar1" "$S/ar2" "$S/ar3" "$S/ar4"; mkdir -p "$S/empty-media"
      # tests build on the newest package with the fixed firmware bundle (boot39dev until boot39 exists)
      TPKG=../build/boot48   # v1.0.1 launch package: boot47 + the pattern-stub kernel check
      check "bad firmware bundle refused (boot37)" 1 "must not ship"     -- $PY make_image_tree.py "$S/badtree" --package ../build/boot37
      check "image tree builds (/opt/ps5win/installer)" 0 "tree OK"     -- $PY make_image_tree.py "$S/imgtree" --package "$TPKG"
      check "tree: only what the image needs"     0 -                     -- $PY -c '
import sys; from pathlib import Path
t = Path(sys.argv[1]) / "installer"
names = {str(f.relative_to(t)) for f in t.rglob("*") if f.is_file() and "payload" not in f.relative_to(t).parts}
assert {"autorun", "autorun.py", "install.py", "deny.txt", "kstub-table.txt", "TREE.txt", "lib/writer.py",
        "ps5win-answers.example.txt"} <= names, names
assert not any(n.startswith(("tests", "proposals")) or n.endswith(".md") for n in names), names
assert (t / "autorun").stat().st_mode & 0o111 and (t / "payload/payload.json").is_file()
assert "USB" not in (t / "deny.txt").read_text().upper()
import re
assert not [str(f) for f in t.rglob("*") if f.is_file() and re.search(rb"(?i)jail.?break|" + b"expl" + b"oit", f.read_bytes())]' "$S/imgtree"
      TT="$S/imgtree/installer"; ARG=(--payload "$TT/payload" --firmware-dir $FWD --countdown 0)
      check "autorun from the tree: Toshiba layout" 0 "TEST MODE"        -- "$TT/autorun" --dry-run --fixture $F/toshiba-image.json --media "$ISO" "${ARG[@]}" --localtime "$U/lt-none" --workdir "$S/ar1/work"
      check "autorun: anti-cheat line on the countdown screen" 0 "Anti-cheat: don.t play online games" -- "$TT/autorun" --dry-run --fixture $F/toshiba-image.json --media "$ISO" "${ARG[@]}" --localtime "$U/lt-none" --workdir "$S/ar4/work"
      check "autorun: no ISO on PS5ISO"           1 "No Windows ISO"     -- "$TT/autorun" --dry-run --fixture $F/toshiba-image.json --media "$S/empty-media" "${ARG[@]}" --workdir "$S/ar2/work"
      check "autorun: boot disk without PS5ISO"   1 "no PS5ISO partition" -- "$TT/autorun" --dry-run --fixture $F/toshiba-noiso.json --media "$ISO" "${ARG[@]}" --workdir "$S/ar3/work"
      check "autorun: real run refused off the console" 1 "PS5's Linux"  -- "$TT/autorun" --media "$ISO"
      check "autorun: PS5ISO source with automount" 0 -                   -- $PY -c '
import sys; sys.path.insert(0, ".")
from autorun import iso_source_devices as f
assert f("systemd-1\n/dev/sda2\n") == ["/dev/sda2"]
assert f("/dev/sda2\n") == ["/dev/sda2"]
assert f("systemd-1\n") == []
assert f("/dev/sda2\n/dev/sdb1\n") == ["/dev/sda2", "/dev/sdb1"]'

      check "kernel check from kstub-table.txt"   0 "MATCH windows_kstub-19041.1288" -- $PY "$TT/check_kernel.py" "$WIM" 2
      check "real run refused off the console"    1 "PS5's Linux, as root" -- $PY install.py --answers $F/answers-install.txt "${IA[@]}" --payload "$S/payload-ov"
      check "image file only for real runs"       1 "real run into a new file" -- $PY install.py --dry-run --answers $F/answers-install.txt --media "$ISO" --image-file "$S/x.img" --firmware-dir $FWD --payload "$S/payload-ov"
    fi
  else
    echo "skip kernel/hive/overlay tests (give the mounted ISO folder, e.g. /Volumes/CES_X64FREO_EN-US_DV9)"
  fi
fi
if [ -n "${ISO22:-}" ] && [ -f "$ISO22/sources/install.wim" ]; then
  echo "== 22H2 media (experimental, ISO22=$ISO22)"
  check "22H2: Pro picked, experimental"      0 "EXPERIMENTAL, image 6" -- $PY find_iso.py "$ISO22"
  check "22H2: Home refused"                  2 "not in this file"      -- $PY find_iso.py --edition Core "$ISO22"
  check "22H2: kernel takes the pattern stub" 0 "PATTERN STUB"          -- $PY check_kernel.py "$ISO22/sources/install.wim" 6
  check "22H2: refused without experimental=yes" 1 "experimental=yes"   -- $PY install.py --dry-run --answers $F/answers-install.txt --media "$ISO22" --fixture $F/two-disk.json --payload-from-build --workdir "$(mktemp -d)"
else
  echo "skip 22H2 checks (ISO22=<mounted Win10 22H2 folder>)"
fi
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
