#!/usr/bin/env python3
"""The registry changes the installer makes BEFORE Windows' first start, applied to copies of the image's hives.

Usage: offline_reg.py <install.wim> <index> <workdir> [--keep]
  Extracts Windows/System32/config/{SYSTEM,SOFTWARE} of that image into <workdir> (wimextract, read-only on the WIM),
  applies the edits below to those copies value by value (libhivex), re-opens them and checks every value, and prints
  what changed. The edited copies are what the installer later adds to the image with the overlay (lib/wimapi.py).
  Never touches a live system or a console disk.

Only things that must be true before the first start live here (the rest is done by SetupComplete.cmd inside
Windows with the same commands that were proven on the console). Source of each line in DESIGN.md, section 4.
"""
import argparse
import hashlib
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "lib"))
from hive import Hive, REG_DWORD, REG_SZ, REG_BINARY  # noqa: E402

# The release list (installer/deny.txt): the PS5's internal NVMe and four AMD functions. No USB IDs: the WinPE-era list
# (winpe/deny.txt) also blocked USB\VID_5910&PID_13FD, an enclosure model, which blocked a user's own drive (run 3).
_DENY_FILE = HERE / "deny.txt"
DENY = [line.strip() for line in _DENY_FILE.read_text().splitlines() if line.strip()]
if any(not d.upper().startswith("PCI\\") for d in DENY):
    raise SystemExit(f"{_DENY_FILE}: only PCI IDs belong in the release deny list")
WU = r"Policies\Microsoft\Windows\WindowsUpdate"
NOWHERE = "http://127.0.0.1:1"


def system_edits(cs):
    c = f"{cs}\\Control"
    s = f"{cs}\\Services"
    return [
        # boot from USB at all (winpe/diag.cmd step 4 set 0x14; the LTSC image already has 0x1C, so OR it in)
        (c, "BootDriverFlags", REG_DWORD, ("or", 0x14)),
        # second wall around the PS5's internal SSD: no inbox NVMe driver (the deny list in SOFTWARE is the first)
        (f"{s}\\stornvme", "Start", REG_DWORD, 4),
        # SAN policy 4 = OfflineInternal: new internal disks stay offline (the internal SSD could never get a letter even
        # if it were seen), USB drives come online with a letter. Was 3 (OfflineAll) up to r18: plugged-in USB drives
        # stayed offline in Disk Management (long-run test 2026-11-23).
        (f"{s}\\partmgr\\Parameters", "SanPolicy", REG_DWORD, 4),
        (f"{c}\\Power", "HibernateEnabled", REG_DWORD, 0),
        (f"{c}\\Session Manager\\Power", "HiberbootEnabled", REG_DWORD, 0),
        # the kernel looks for the boot partition ONCE unless this is set (ms), then retries every 200 ms: without it
        # every start races the USB disk's enumeration -> random 0x7B INACCESSIBLE_BOOT_DEVICE C0000034 (2026-11-24)
        (f"{c}\\PnP", "PollBootPartitionTimeout", REG_DWORD, 60000),
        (f"{c}\\CrashControl", "AutoReboot", REG_DWORD, 1),   # 1.1: stop error -> restart -> warm restart loop
        (f"{c}\\CrashControl", "CrashDumpEnabled", REG_DWORD, 3),
        (f"{c}\\CrashControl", "DisplayParameters", REG_DWORD, 1),
        # Windows Update off from the very first start: the kernel stub is tied to this ntoskrnl build
        (f"{s}\\wuauserv", "Start", REG_DWORD, 4),
        (f"{s}\\UsoSvc", "Start", REG_DWORD, 4),
        (f"{s}\\WaaSMedicSvc", "Start", REG_DWORD, 4),
        # Delivery Optimization: peer-to-peer update sharing, listened on TCP 7680 on run 4 (security report)
        (f"{s}\\DoSvc", "Start", REG_DWORD, 4),
    ]


def software_edits():
    r = r"Policies\Microsoft\Windows\DeviceInstall\Restrictions"
    e = [(r, "DenyDeviceIDs", REG_DWORD, 1), (r, "DenyDeviceIDsRetroactive", REG_DWORD, 1)]
    e += [(r + r"\DenyDeviceIDs", str(i), REG_SZ, dev) for i, dev in enumerate(DENY, 1)]
    e += [(WU + r"\AU", n, REG_DWORD, v) for n, v in
          (("NoAutoUpdate", 1), ("AUOptions", 1), ("UseWUServer", 1), ("NoAutoRebootWithLoggedOnUsers", 1))]
    e += [(WU, n, REG_DWORD, 1) for n in ("DoNotConnectToWindowsUpdateInternetLocations", "DisableWindowsUpdateAccess",
                                          "ExcludeWUDriversInQualityUpdate", "SetDisableUXWUAccess")]
    e += [(WU, n, REG_SZ, NOWHERE) for n in ("WUServer", "WUStatusServer", "UpdateServiceUrlAlternate")]
    # Delivery Optimization off by policy as well (0 = HTTP only, no peers), in case anything restarts the service
    e += [(r"Policies\Microsoft\Windows\DeliveryOptimization", "DODownloadMode", REG_DWORD, 0)]
    # no driver downloads from Windows Update for our devices
    e += [(r"Microsoft\Windows\CurrentVersion\DriverSearching", "SearchOrderConfig", REG_DWORD, 0)]
    # Edge's GPU acceleration flashes the whole window white on our driver (2026-11-24, owner: many bright flashes);
    # policy off from the first start. Edge then shows "managed by your organization" (README says why).
    e += [(r"Policies\Microsoft\Edge", "HardwareAccelerationModeEnabled", REG_DWORD, 0)]
    return e


def cert_blob(der):
    """Windows' serialized certificate entry, as certutil writes it (checked against the image's own ROOT store):
    (property id, 1, length, data) records, the certificate itself (id 32) last. SHA-1 (id 3) is enough besides it;
    Windows fills in the rest when it reads the store."""
    sha1 = hashlib.sha1(der).digest()
    return struct.pack("<III", 3, 1, 20) + sha1 + struct.pack("<III", 32, 1, len(der)) + der


def cert_edits(cert_paths):
    """Trust our test-signing certificate(s) machine-wide from the first boot: LocalMachine Root and TrustedPublisher,
    so test-signed driver packages install without the 'unverified publisher' prompt (Toshiba run 3 showed it)."""
    e = []
    for c in cert_paths or []:
        der = Path(c).read_bytes()
        if der[:1] != b"\x30":
            raise SystemExit(f"{c}: not a DER certificate")
        thumb = hashlib.sha1(der).hexdigest().upper()
        for store in ("ROOT", "TrustedPublisher"):
            e.append((rf"Microsoft\SystemCertificates\{store}\Certificates\{thumb}", "Blob", REG_BINARY, cert_blob(der)))
    return e


def current_control_set(path):
    with Hive(path) as h:
        cur = h.get("Select", "Current")
    if not cur or cur[0] != REG_DWORD:
        raise SystemExit("SYSTEM hive has no Select\\Current")
    return f"ControlSet{cur[1]:03d}"


def apply(path, edits):
    changes, skipped = [], []
    with Hive(path, write=True) as h:
        for i, (key, name, t, v) in enumerate(edits):
            if "\\Services\\" in key and name == "Start" and not h.node(key):
                skipped.append(key)                # never invent a service the image does not have
                continue
            before = h.get(key, name)
            if isinstance(v, tuple) and v[0] == "or":
                v = (before[1] if before and before[0] == t else 0) | v[1]
                edits[i] = (key, name, t, v)
            h.set(key, name, t, v)
            changes.append((key, name, before, (t, v)))
        h.commit()
    with Hive(path) as h:              # re-open and check every value
        for key, name, t, v in edits:
            if key in skipped:
                continue
            got = h.get(key, name)
            if got != (t, v):
                raise SystemExit(f"VERIFY FAILED {key}\\{name}: {got!r} != {(t, v)!r}")
    for key in skipped:
        print(f"  skipped (service not in this image): {key}")
    return changes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wim")
    ap.add_argument("index")
    ap.add_argument("workdir")
    ap.add_argument("--cert", action="append", help="DER certificate to trust machine-wide (Root + TrustedPublisher)")
    a = ap.parse_args()
    wd = Path(a.workdir)
    wd.mkdir(parents=True, exist_ok=True)
    for name in ("SYSTEM", "SOFTWARE"):
        (wd / name).unlink(missing_ok=True)
    subprocess.run(["wimextract", a.wim, a.index, "/Windows/System32/config/SYSTEM",
                    "/Windows/System32/config/SOFTWARE", f"--dest-dir={wd}", "--no-acls", "--quiet"], check=True)
    cs = current_control_set(wd / "SYSTEM")
    total = 0
    for name, edits in (("SYSTEM", system_edits(cs)), ("SOFTWARE", software_edits() + cert_edits(a.cert))):
        print(f"== {name} ({wd / name})" + (f", current control set {cs}" if name == "SYSTEM" else ""))
        for key, val, before, after in apply(wd / name, edits):
            b = "absent" if before is None else f"{before[1]!r}"
            a_ = f"{len(after[1])}-byte certificate entry" if isinstance(after[1], bytes) else repr(after[1])
            print(f"  {key}\\{val}: {b} -> {a_}")
            total += 1
    print(f"{total} values set and verified on the copies in {wd}")


if __name__ == "__main__":
    main()
