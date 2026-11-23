#!/usr/bin/env python3
"""Collect the C:\\ps5 payload from this repo's build outputs into one staging tree, check it, and write a manifest.

Usage: build_payload.py OUTDIR [--firmware-dir DIR] [--clean]
  OUTDIR/ps5/...           the tree the installer adds to the image as C:\\ps5 (lib/wimapi.py overlay)
  OUTDIR/ps5/setup/MANIFEST.sha256, OUTDIR/payload.json
  --firmware-dir DIR       TEST ONLY: also stage gpu\\fw\\cyan_skillfish2_*.bin from DIR (the real installer copies
                           them from the Linux image at install time; they are not part of the payload)
  --frozen PAYLOAD.json    release lock: every staged file must match the hashes recorded in that earlier build
  --clean                  empty OUTDIR first (only if it holds a previous payload: payload.json or payload.partial)
Checks (any failure = exit 1, nothing half-written is left marked as good):
  - every manifest source exists; no "unsigned"/"-vm"/test binaries
  - kernel drivers: AMD64, native subsystem, no HIGH_ENTROPY_VA / TERMINAL_SERVER_AWARE (Windows 10 refuses those:
    0xC0000018), an Authenticode signature present
  - each INF's CatalogFile is staged next to it; ps5wddm.inf's user-mode driver path is staged
  - DLL machine type matches its folder (vk32/d3d32/x86 loader = i386, the rest AMD64)
  - Vulkan manifests point at a DLL that is staged next to them
  - every DLL a staged program or library imports is staged in the same folder, or is a Windows DLL known to be on a
    clean 19044.1288 install (a missing libwinpthread-1.dll kept the Vulkan driver from loading on every fresh
    install until 2026-11-23: the compositor could not use the GPU and Windows stopped the display adapter)
  - scripts get CRLF line endings
  - every C:\\ps5\\... path named in the staged scripts and tasks exists in the tree, or is a runtime file
  - privacy: no file holds the user's network names, access-point MACs or the console's Wi-Fi MAC (needles read in
    memory from ps5-hackintosh/kexts/common/mt7921_core.h; values are never printed)
"""
import argparse
import hashlib
import json
import re
import shutil
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "lib"))
WH = HERE.parent                                  # windows-handoff/
MANIFEST = HERE / "payload" / "manifest.txt"
FORBIDDEN = re.compile(r"(unsigned|-vm\b|-vm\.|tri\.exe|test|dxsurf|gdianim|stall|suspend)", re.I)
I386, AMD64 = 0x14C, 0x8664
# C:\ps5 paths the scripts name that are made at run time (logs, markers, the microcode copied at install)
RUNTIME = re.compile(r"^(.*\.(log|txt|tmp|bmp|count|done)|gpu\\fw|gpu\\fw\\.*|gpu\\umd-.*|gpu\\autostart\.(off|trying|nofull)"
                     r"|gpu\\recover\.count|gpu\\ping\.tmp|gpu\\vk\\demo\.cmd|gpu\\dgvoodoo.*|gpu\\kit\\.*\.txt"
                     r"|gpu\\ps5wddm-trace.*|setup\\done\.txt|dumps.*|wu-off\.ps1|gpu\\start\.log\.?)$", re.I)

TASK_XML = """<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>{desc}</Description>
    <URI>\\{name}</URI>
  </RegistrationInfo>
  <Triggers>
    <BootTrigger>
      <Enabled>true</Enabled>
    </BootTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
      <UserId>S-1-5-18</UserId>
      <RunLevel>HighestAvailable</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate>
    <StartWhenAvailable>false</StartWhenAvailable>
    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
    <IdleSettings>
      <StopOnIdleEnd>false</StopOnIdleEnd>
      <RestartOnIdle>false</RestartOnIdle>
    </IdleSettings>
    <AllowStartOnDemand>true</AllowStartOnDemand>
    <Enabled>true</Enabled>
    <Hidden>false</Hidden>
    <RunOnlyIfIdle>false</RunOnlyIfIdle>
    <WakeToRun>false</WakeToRun>
    <ExecutionTimeLimit>{limit}</ExecutionTimeLimit>
    <Priority>7</Priority>{restart}
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>{command}</Command>
    </Exec>
  </Actions>
</Task>
"""
TASKS = {
    # autostart.cmd waits 30 s itself (a trigger delay made the task never fire, 2026-11-20); it ends after the GPU start
    "setup\\tasks\\ps5-gpu-autostart.xml": dict(
        name="ps5-gpu-autostart", command="C:\\ps5\\gpu\\autostart.cmd", limit="PT1H", restart="",
        desc="PS5: start the GPU (shim, microcode, ring) after boot, then the full display driver. Latch: autostart.off"),
    # the watchdog loops forever: no time limit, and Windows restarts it if it ever exits with an error
    "setup\\tasks\\ps5-guard.xml": dict(
        name="ps5-guard", command="C:\\ps5\\ps5guard.cmd", limit="PT0S",
        restart="\n    <RestartOnFailure>\n      <Interval>PT1M</Interval>\n      <Count>3</Count>\n    </RestartOnFailure>",
        desc="PS5: temperature and GPU watchdog (fan to full at 80 C, ends GPU programs at 88 C, recovers a stuck GPU)"),
}


class Fail(Exception):
    pass


# Catalogs allowed to ship although their members are not in ascending order (hash-pinned; nothing else gets through).
UNSORTED_CAT_EXCEPTIONS = {}   # the r7 driver package (0314ff55) left the payload on 2026-11-23


# The user's own network details must never be in the payload (their words, 2026-11-21: no Wi-Fi key, no private details
# of the Wi-Fi). The MT7921 core shared with macOS carries recorded join frames: network names, the access points'
# MACs, the console's Wi-Fi MAC. They are read here in memory only, as needles; no value is ever printed or written.
PRIVATE_SRC = WH.parent / "ps5-hackintosh/kexts/common/mt7921_core.h"


def private_needles():
    if not PRIVATE_SRC.is_file():
        raise Fail(f"privacy check needs {PRIVATE_SRC} (the source of the network details to keep out)")
    src = PRIVATE_SRC.read_text(errors="replace")
    needles = []                                          # (label, bytes)
    for i, ssid in enumerate(re.findall(r'\{\s*"([^"]+)"\s*,\s*mt_\w+_bssid', src), 1):
        needles += [(f"network name #{i}", ssid.encode()), (f"network name #{i}", ssid.encode("utf-16-le"))]
    def arr(name):
        m = re.search(r"static const uint8_t %s\[\d+\]\s*=\s*\{([^}]*)\}" % name, src)
        return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1))) if m else b""
    macs = {}
    for i, name in enumerate(re.findall(r"static const uint8_t (mt_\w+_bssid)\[6\]", src), 1):
        macs[f"access point MAC #{i}"] = arr(name)
    for name in re.findall(r"static const uint8_t (mt_\w+_assoc_req_wpa2)\[", src):
        frame = arr(name)
        if len(frame) >= 16:
            macs["console Wi-Fi MAC"] = frame[10:16]     # 802.11 header: addr2 = the sender (this console)
    for label, mac in macs.items():
        if len(mac) != 6:
            continue
        h = mac.hex()
        needles += [(label, mac)] + [(label, s.encode(e)) for s in
                    (h, ":".join(h[k:k + 2] for k in range(0, 12, 2)), "-".join(h[k:k + 2] for k in range(0, 12, 2)))
                    for e in ("utf-8", "utf-16-le")]
    if not any(l.startswith("network name") for l, _ in needles):
        raise Fail("privacy check: found no network names in the MT7921 core (format changed?)")
    # the dev disks' identities (serials, disk and partition GUIDs), read from dev files that hold them (the old
    # hard-coded launch script, the dev HDD's saved partition table); found in DESIGN.md on 2026-11-22
    for f in [WH / "build/boot32/run_stage.sh"] + sorted((WH / "build/wininstall-backup").glob("*.sfdisk")):
        if not f.is_file():
            continue
        text = f.read_text(errors="replace")
        ids = set(re.findall(r"\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b", text))
        for line in text.splitlines():               # serials: mixed letter+digit tokens on lines about SERIAL
            if "SERIAL" in line.upper():
                ids |= set(re.findall(r"\b(?=[0-9A-Z]*[0-9])(?=[0-9A-Z]*[A-Z])[0-9A-Z]{8,20}\b", line))
        ids -= {"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"}   # standard types
        for i in ids:
            if i.upper() in {"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7",
                             "E3C9E316-0B5C-4DB8-817D-F92DF00215AE"}:
                continue
            needles += [("dev disk identifier", i.encode()), ("dev disk identifier", i.lower().encode()),
                        ("dev disk identifier", i.upper().encode()), ("dev disk identifier", i.encode("utf-16-le"))]
    # the build machine's user name (debug paths, e.g. a PDB path, carry it; found in ps5wddm.sys on 2026-11-21)
    import getpass
    for name in {getpass.getuser(), Path.home().name}:
        if len(name) >= 4:
            needles += [("build machine user name", name.encode()), ("build machine user name", name.encode("utf-16-le"))]
    return needles


def privacy_scan(staged):
    needles = private_needles()
    hits = []
    for key, (dest, src, kind, data) in staged.items():
        low = data.lower()
        for label, n in needles:
            if n in data or (n.isascii() and n.lower() in low):
                hits.append(f"{dest}: {label}")
    if hits:
        raise Fail("private network details in the payload (values not shown):\n  " + "\n  ".join(sorted(set(hits))))
    return len({l for l, _ in needles})


def read_manifest(path=None):
    rows = []
    for n, line in enumerate(Path(path or MANIFEST).read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split("|")]
        if len(parts) != 3:
            raise Fail(f"manifest line {n}: expected 3 fields")
        rows.append(tuple(parts))
    return rows


def pe_info(data):
    if data[:2] != b"MZ":
        raise Fail("not a PE file")
    e = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e:e + 4] != b"PE\0\0":
        raise Fail("no PE signature")
    machine = struct.unpack_from("<H", data, e + 4)[0]
    opt = e + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    subsystem, dllchar = struct.unpack_from("<HH", data, opt + 68)
    dd = opt + (112 if magic == 0x20B else 96)
    sec_off, sec_size = struct.unpack_from("<II", data, dd + 8 * 4)     # data directory 4 = certificate table
    return {"machine": machine, "subsystem": subsystem, "dllchar": dllchar, "signed": sec_off != 0 and sec_size > 8}


def authenticode(data, alg):
    """PE Authenticode digest: the file without its checksum field, certificate-table entry and certificate table."""
    e = struct.unpack_from("<I", data, 0x3C)[0]
    opt = e + 24
    dd = opt + (112 if struct.unpack_from("<H", data, opt)[0] == 0x20B else 96)
    off = struct.unpack_from("<I", data, dd + 32)[0]
    h = hashlib.new(alg)
    h.update(data[:opt + 64])
    h.update(data[opt + 68:dd + 32])
    h.update(data[dd + 40:off if off else len(data)])
    return h.digest()


# Windows' own DLLs our binaries may import by name: each one checked on a clean 19044.1288 install (the Patriot's r14
# install, System32 / SysWOW64 / drivers, 2026-11-23). API sets (api-ms-*, ext-ms-*) are resolved by Windows itself.
# Anything else must be staged next to the binary that imports it. A new name: check it exists on a clean install of
# the supported Windows, for the binary's bitness, before adding it here.
SYSTEM_DLLS = {"advapi32.dll", "cfgmgr32.dll", "d3d11.dll", "d3d9.dll", "dxgi.dll", "gdi32.dll", "kernel32.dll",
               "newdev.dll", "setupapi.dll", "user32.dll", "version.dll"}
KERNEL_IMPORTS = {"ntoskrnl.exe", "hal.dll", "ndis.sys"}


def pe_imports(data):
    """Names in the import table and the delay-import table of a PE file, lower case."""
    e = struct.unpack_from("<I", data, 0x3C)[0]
    nsec, optsize = struct.unpack_from("<H", data, e + 6)[0], struct.unpack_from("<H", data, e + 20)[0]
    opt = e + 24
    dd = opt + (112 if struct.unpack_from("<H", data, opt)[0] == 0x20B else 96)
    secs = [struct.unpack_from("<IIII", data, opt + optsize + 40 * i + 8) for i in range(nsec)]

    def off(rva):
        for vsize, va, rsize, raw in secs:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        raise Fail("import table points outside the file's sections")

    def names(rva, step, name_at):
        out = []
        o = off(rva) if rva else None
        while o is not None:
            n = struct.unpack_from("<I", data, o + name_at)[0]
            if not n:
                break
            s = off(n)
            out.append(data[s:data.index(b"\0", s)].decode("ascii", "replace").lower())
            o += step
        return out

    return names(struct.unpack_from("<I", data, dd + 8)[0], 20, 12) + names(struct.unpack_from("<I", data, dd + 13 * 8)[0], 32, 4)


def check_driver(dest, data):
    p = pe_info(data)
    if p["machine"] != AMD64:
        raise Fail(f"{dest}: not AMD64 (0x{p['machine']:x})")
    if p["subsystem"] != 1:
        raise Fail(f"{dest}: subsystem {p['subsystem']}, not native")
    # lld-link drivers without /highentropyva:no /tsaware:no were refused (0xC0000018, 2026-11-19). ps5gpu.sys (mingw)
    # has HIGH_ENTROPY_VA and loads fine, so only TERMINAL_SERVER_AWARE is fatal; HIGH_ENTROPY_VA is reported.
    if p["dllchar"] & 0x8000:
        raise Fail(f"{dest}: TERMINAL_SERVER_AWARE set (link with /tsaware:no)")
    if not p["signed"]:
        raise Fail(f"{dest}: no Authenticode signature")
    return [f"{dest}: HIGH_ENTROPY_VA set (accepted for ps5gpu.sys today)"] if p["dllchar"] & 0x0020 else []


def want_machine(dest):
    d = dest.lower()
    if d.endswith(".dll") or d.endswith(".exe"):
        return I386 if (d.startswith(("gpu\\vk32\\", "gpu\\d3d32\\")) or d.endswith("x86.dll")) else AMD64
    return None


def crlf(data):
    return re.sub(rb"\r?\n", b"\r\n", data)


def stage(out, firmware_dir, manifest=None):
    root = out / "ps5"
    rows = read_manifest(manifest)
    staged, notes = {}, []
    for dest, src, kind in rows:
        target = root / dest.replace("\\", "/")
        target.parent.mkdir(parents=True, exist_ok=True)
        if kind == "generated":
            t = TASKS.get(dest)
            if t is None:
                raise Fail(f"{dest}: no generator")
            data = ("\ufeff" + TASK_XML.format(**t).replace("\n", "\r\n")).encode("utf-16-le")
        else:
            s = WH / src
            if not s.is_file():
                raise Fail(f"{dest}: source missing: {src}")
            if FORBIDDEN.search(Path(src).name) and Path(src).suffix.lower() in (".sys", ".exe", ".dll", ".cat"):
                raise Fail(f"{dest}: source looks like a test/unsigned build: {src}")
            data = s.read_bytes()
            if kind == "script":
                data = crlf(data)
            elif kind == "driver":
                notes += check_driver(dest, data)
            elif kind == "signed-cat":
                if len(data) < 512 or data[0] != 0x30:
                    raise Fail(f"{dest}: not a DER catalog")
            m = want_machine(dest)
            if m is not None and pe_info(data)["machine"] != m:
                raise Fail(f"{dest}: machine 0x{pe_info(data)['machine']:x}, want 0x{m:x}")
        if b"PRIVATE KEY-----" in data or b"PRIVATE KEY" in data[:64]:
            raise Fail(f"{dest}: contains a private key; never ship one")
        target.write_bytes(data)
        staged[dest.lower()] = (dest, src, kind, data)
    if firmware_dir:
        fw = sorted(Path(firmware_dir).glob("cyan_skillfish2_*.bin"))
        if len(fw) != 7:
            raise Fail(f"--firmware-dir: expected 7 cyan_skillfish2_*.bin, found {len(fw)}")
        for f in fw:
            d = f"gpu\\fw\\{f.name}"
            (root / "gpu/fw").mkdir(parents=True, exist_ok=True)
            (root / "gpu/fw" / f.name).write_bytes(f.read_bytes())
            staged[d.lower()] = (d, str(f), "test-only", f.read_bytes())
    return root, staged, notes


def check_tree(root, staged, notes):
    # INF catalogs and the UMD path
    for key, (dest, *_r, data) in list(staged.items()):
        if key.endswith(".inf"):
            text = data.decode("utf-8", "replace")
            m = re.search(r"^\s*CatalogFile\s*=\s*(\S+)", text, re.M | re.I)
            if not m:
                raise Fail(f"{dest}: no CatalogFile")
            cat = dest.rsplit("\\", 1)[0] + "\\" + m.group(1)
            if cat.lower() not in staged:
                raise Fail(f"{dest}: catalog {m.group(1)} not staged")
            ver = re.search(r"^\s*DriverVer\s*=\s*(\S+)", text, re.M | re.I)
            notes.append(f"{dest}: DriverVer {ver.group(1) if ver else '?'}")
            for umd in re.findall(r"UserModeDriverName,0x00010000,\"([^\"]+)\"", text):
                rel = umd.lower().removeprefix("c:\\ps5\\")
                if rel not in staged:
                    raise Fail(f"{dest}: user-mode driver {umd} not staged")
    # every driver catalog must carry the certificate we ship and trust (offline_reg.py --cert): a catalog signed with
    # another certificate would bring back the "unverified publisher" prompt and block the unattended setup
    cer = staged.get("gpu\\wddm\\ps5test.cer")
    if not cer:
        raise Fail("gpu\\wddm\\ps5test.cer (the test-signing certificate) is not staged")
    for key, (dest, src, kind, data) in staged.items():
        if kind == "signed-cat" and cer[3] not in data:
            raise Fail(f"{dest}: not signed with the shipped certificate gpu\\wddm\\ps5test.cer")
    for key, (dest, src, kind, data) in staged.items():
        if kind == "driver" and cer[3] not in data:
            raise Fail(f"{dest}: its signature does not carry the shipped certificate gpu\\wddm\\ps5test.cer")
    # catalogs, exactly: each lists the .sys and .inf files in its folder, under every digest algorithm it uses (ours:
    # SHA-1 and SHA-256), nothing else (no stale entries from an older driver), and in ascending tag order (Windows
    # looks members up by binary search: a hash-swapped catalog in the old order was rejected by 19044.7725 with
    # 0xE000024B, 2026-11-22). Drivers use their Authenticode digest, the INF its flat file hash.
    import catalog
    for key, (dest, src, kind, data) in staged.items():
        if kind != "signed-cat":
            continue
        folder = key.rsplit("\\", 1)[0]
        try:
            mem = catalog.members(data)
        except (catalog.CatError, StopIteration, IndexError) as e:
            raise Fail(f"{dest}: cannot read the catalog ({e})")
        tags = [t for t, _ in mem]
        algs = sorted({catalog.ALG_BY_LEN.get(len(t), "?") for t in tags})
        if "?" in algs or "sha256" not in algs:
            raise Fail(f"{dest}: unexpected member tags (algorithms {algs})")
        if catalog.unsorted(data):
            why = UNSORTED_CAT_EXCEPTIONS.get(hashlib.sha256(data).hexdigest())
            if not why:
                raise Fail(f"{dest}: member tags are not in ascending order (Windows binary-searches them): re-make the "
                           "catalog with New-FileCatalog/makecat instead of patching hashes in place")
            notes.append(f"{dest}: members NOT in ascending order, shipped by pinned exception ({why})")
        want = {}
        for k2, (d2, s2, kind2, data2) in staged.items():
            if k2.rsplit("\\", 1)[0] != folder:
                continue
            if kind2 == "driver":
                want[d2] = {a: authenticode(data2, a) for a in algs}
            elif k2.endswith(".inf"):
                want[d2] = {a: hashlib.new(a, data2).digest() for a in algs}
        for d2, digests in want.items():
            for a, h in digests.items():
                if h not in tags:
                    raise Fail(f"{dest}: does not list {d2}'s {a.upper()} digest {h.hex().upper()} "
                               "(re-make the catalog after changing the file)")
        expected = {h for digests in want.values() for h in digests.values()}
        stale = [catalog.tag_text(t) for t in tags if t not in expected]
        if stale:
            raise Fail(f"{dest}: lists {len(stale)} member(s) matching no shipped file (stale): {', '.join(stale)}")
    # imports: nothing may depend on a DLL that a clean Windows lacks and we do not ship next to it
    nimports = 0
    for key, (dest, src, kind, data) in staged.items():
        if not key.endswith((".dll", ".exe", ".sys")):
            continue
        folder = key.rsplit("\\", 1)[0] + "\\" if "\\" in key else ""
        allowed = KERNEL_IMPORTS if key.endswith(".sys") else SYSTEM_DLLS
        try:
            names = pe_imports(data)
        except (struct.error, ValueError) as e:
            raise Fail(f"{dest}: cannot read its import table ({e})")
        for name in names:
            nimports += 1
            if name.startswith(("api-ms-", "ext-ms-")) or name in allowed or folder + name in staged:
                continue
            raise Fail(f"{dest}: imports {name}, which is neither staged in the same folder nor a known Windows DLL "
                       "(ship it next to the file, or verify it on a clean install and add it to SYSTEM_DLLS)")
    notes.append(f"imports: {nimports} DLL imports resolved (same folder, API set or known Windows DLL)")
    for key, (dest, *_r, data) in staged.items():
        if key.endswith("_icd.json"):
            lib = json.loads(data)["ICD"]["library_path"]
            rel = dest.rsplit("\\", 1)[0] + "\\" + lib.lstrip(".\\")
            if rel.lower() not in staged and not key.startswith("gpu\\vkreg\\"):
                raise Fail(f"{dest}: library {lib} not staged next to it")
    # every C:\ps5\... path in scripts and tasks
    refs = set()
    for key, (dest, src, kind, data) in staged.items():
        if kind in ("script", "generated"):
            text = data.decode("utf-16-le") if kind == "generated" else data.decode("utf-8", "replace")
            for m in re.finditer(r"C:\\ps5\\([A-Za-z0-9_.\\-]+)", text, re.I):
                refs.add((m.group(1).rstrip(".\\"), dest))
    missing = []
    for ref, where in sorted(refs):
        r = ref.lower()
        is_dir = any(k.startswith(r + "\\") for k in staged)
        if r not in staged and not is_dir and not RUNTIME.match(r):
            missing.append(f"C:\\ps5\\{ref} (named in {where})")
    if missing:
        raise Fail("paths named in scripts but not in the payload:\n  " + "\n  ".join(missing))
    return notes, len(refs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir")
    ap.add_argument("--firmware-dir")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--manifest", help="another manifest (tests)")
    ap.add_argument("--frozen", help="a payload.json from an earlier build: refuse if any staged file differs from it "
                                     "(release lock: freeze once the GPU session calls its binaries final)")
    a = ap.parse_args()
    out = Path(a.outdir).resolve()
    if HERE in out.parents or out == HERE:
        sys.exit("OUTDIR must not be inside installer/ (use build/ or a scratch directory)")
    if out.exists() and any(out.iterdir()):
        ours = (out / "payload.json").exists() or (out / "payload.partial").exists()
        if not (a.clean and ours):
            sys.exit(f"{out} is not empty (use --clean on a previous payload directory)")
        shutil.rmtree(out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "payload.partial").write_text("build_payload.py did not finish\n")
    try:
        root, staged, notes = stage(out, a.firmware_dir, a.manifest)
        nprivate = privacy_scan(staged)              # first: private details matter more than anything else
        notes, nrefs = check_tree(root, staged, notes)
    except Fail as e:
        print(f"PAYLOAD FAILED: {e}")
        sys.exit(1)
    lines, total = [], 0
    for key in sorted(staged):
        dest, src, kind, data = staged[key]
        lines.append(f"{hashlib.sha256(data).hexdigest()}  {dest}")
        total += len(data)
    (root / "setup/MANIFEST.sha256").write_text("\r\n".join(lines) + "\r\n")
    info = {"files": len(staged), "bytes": total, "test_only_firmware": bool(a.firmware_dir),
            "sources": {staged[k][0]: staged[k][1] for k in sorted(staged)},
            "sha256": {staged[k][0]: hashlib.sha256(staged[k][3]).hexdigest() for k in sorted(staged)},
            "driver_versions": [n for n in notes if "DriverVer" in n]}
    if a.frozen:
        want = json.loads(Path(a.frozen).read_text()).get("sha256", {})
        diff = sorted(set(want) ^ set(info["sha256"])) + sorted(k for k in want if k in info["sha256"]
                                                               and want[k] != info["sha256"][k])
        if diff:
            print("PAYLOAD FAILED: differs from the frozen payload " + a.frozen + ":\n  " + "\n  ".join(diff))
            sys.exit(1)
        print(f"matches the frozen payload {a.frozen}")
    (out / "payload.json").write_text(json.dumps(info, indent=1))
    (out / "payload.partial").unlink()
    for n in notes:
        print(n)
    print(f"privacy check: none of {nprivate} private details (network names/MACs from the MT7921 core, dev disk "
          "serials/GUIDs, the build machine's user name; never shown) in any file")
    print(f"payload OK: {len(staged)} files, {total / 2**20:.1f} MiB in {root}; {nrefs} C:\\ps5 paths in scripts checked"
          + ("; gpu\\fw staged for TESTS ONLY" if a.firmware_dir else "; gpu\\fw is filled at install time"))


if __name__ == "__main__":
    main()
