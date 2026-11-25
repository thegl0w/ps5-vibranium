#!/usr/bin/env python3
"""Generate Windows\\Panther\\unattend.xml for the PS5 install: no screens at the first start, a local administrator
that logs on by itself, nothing personal built in.

Usage: make_unattend.py --out FILE [--answers FILE] [--language en-US] [--localtime /etc/localtime]
                        [--hive SOFTWARE] [--json]
  --answers   the installer's KEY=VALUE answer file (the same file plan_layout.py reads). Keys used here, all optional:
                computer_name (default PS5WIN), user_name (default ps5), password (default: none),
                time_zone (a Windows time-zone ID; default: from --localtime), locale (default: --language),
                product_key (default: none; Windows stays unactivated)
  --language  the image's language (find_iso.py reports it from the WIM XML), default en-US
  --localtime the Linux zone file: its symlink target names the IANA zone, mapped to a Windows ID (UTC when unknown)
  --hive      a copy of the image's SOFTWARE hive: the time zone must be one the image knows (offline_reg.py makes one)
The password and product key are never printed (shown as set/not set); in the XML the password is in Windows'
usual base64 form, which is NOT encryption. C:\\ps5\\firstlogon.cmd deletes the unattend file after the first logon.
"""
import argparse
import base64
import json
import os
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))

NS = "urn:schemas-microsoft-com:unattend"
WCM = "http://schemas.microsoft.com/WMIConfig/2002/State"
COMMON = {"processorArchitecture": "amd64", "publicKeyToken": "31bf3856ad364e35", "language": "neutral",
          "versionScope": "nonSxS"}
# IANA zone -> Windows time-zone ID (common zones; every value is checked against the image's own list in the tests)
TZ = {
    "UTC": "UTC", "Etc/UTC": "UTC", "Etc/GMT": "UTC", "Universal": "UTC", "Zulu": "UTC",
    "America/New_York": "Eastern Standard Time", "America/Toronto": "Eastern Standard Time",
    "America/Detroit": "Eastern Standard Time", "America/Montreal": "Eastern Standard Time",
    "America/Chicago": "Central Standard Time", "America/Winnipeg": "Central Standard Time",
    "America/Denver": "Mountain Standard Time", "America/Edmonton": "Mountain Standard Time",
    "America/Phoenix": "US Mountain Standard Time", "America/Los_Angeles": "Pacific Standard Time",
    "America/Vancouver": "Pacific Standard Time", "America/Anchorage": "Alaskan Standard Time",
    "Pacific/Honolulu": "Hawaiian Standard Time", "America/Halifax": "Atlantic Standard Time",
    "America/St_Johns": "Newfoundland Standard Time", "America/Regina": "Canada Central Standard Time",
    "America/Mexico_City": "Central Standard Time (Mexico)", "America/Sao_Paulo": "E. South America Standard Time",
    "America/Argentina/Buenos_Aires": "Argentina Standard Time", "America/Bogota": "SA Pacific Standard Time",
    "America/Lima": "SA Pacific Standard Time", "America/Santiago": "Pacific SA Standard Time",
    "Europe/London": "GMT Standard Time", "Europe/Dublin": "GMT Standard Time", "Europe/Lisbon": "GMT Standard Time",
    "Europe/Paris": "Romance Standard Time", "Europe/Brussels": "Romance Standard Time",
    "Europe/Madrid": "Romance Standard Time", "Europe/Copenhagen": "Romance Standard Time",
    "Europe/Berlin": "W. Europe Standard Time", "Europe/Amsterdam": "W. Europe Standard Time",
    "Europe/Rome": "W. Europe Standard Time", "Europe/Vienna": "W. Europe Standard Time",
    "Europe/Zurich": "W. Europe Standard Time", "Europe/Stockholm": "W. Europe Standard Time",
    "Europe/Oslo": "W. Europe Standard Time", "Europe/Warsaw": "Central European Standard Time",
    "Europe/Prague": "Central Europe Standard Time", "Europe/Budapest": "Central Europe Standard Time",
    "Europe/Athens": "GTB Standard Time", "Europe/Bucharest": "GTB Standard Time",
    "Europe/Helsinki": "FLE Standard Time", "Europe/Kiev": "FLE Standard Time", "Europe/Kyiv": "FLE Standard Time",
    "Europe/Istanbul": "Turkey Standard Time", "Europe/Moscow": "Russian Standard Time",
    "Asia/Dubai": "Arabian Standard Time", "Asia/Riyadh": "Arab Standard Time", "Asia/Tehran": "Iran Standard Time",
    "Asia/Jerusalem": "Israel Standard Time", "Asia/Karachi": "Pakistan Standard Time",
    "Asia/Kolkata": "India Standard Time", "Asia/Calcutta": "India Standard Time",
    "Asia/Dhaka": "Bangladesh Standard Time", "Asia/Bangkok": "SE Asia Standard Time",
    "Asia/Jakarta": "SE Asia Standard Time", "Asia/Singapore": "Singapore Standard Time",
    "Asia/Shanghai": "China Standard Time", "Asia/Hong_Kong": "China Standard Time",
    "Asia/Taipei": "Taipei Standard Time", "Asia/Seoul": "Korea Standard Time", "Asia/Tokyo": "Tokyo Standard Time",
    "Australia/Perth": "W. Australia Standard Time", "Australia/Adelaide": "Cen. Australia Standard Time",
    "Australia/Brisbane": "E. Australia Standard Time", "Australia/Sydney": "AUS Eastern Standard Time",
    "Australia/Melbourne": "AUS Eastern Standard Time", "Pacific/Auckland": "New Zealand Standard Time",
    "Africa/Johannesburg": "South Africa Standard Time", "Africa/Cairo": "Egypt Standard Time",
    "Africa/Lagos": "W. Central Africa Standard Time", "Africa/Nairobi": "E. Africa Standard Time",
}
RESERVED_USERS = {"administrator", "guest", "defaultaccount", "wdagutilityaccount", "system", "none"}


class Bad(Exception):
    pass


def read_answers(path):
    kv = {}
    if path:
        for line in Path(path).read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                kv[k.strip()] = v.strip()
    return kv


def iana_zone(localtime):
    p = Path(localtime)
    if not p.is_symlink():
        return None
    target = os.readlink(p)
    m = re.search(r"zoneinfo/(.+)$", target)
    return m.group(1) if m else None


def image_zones(hive):
    if not hive:
        return None
    from hive import L, Hive        # libhivex binding (lib/hive.py)
    import ctypes
    out = set()
    with Hive(hive) as h:
        n = h.node(r"Microsoft\Windows NT\CurrentVersion\Time Zones")
        if not n:
            raise Bad("the hive has no Time Zones key")
        L.hivex_node_children.restype = ctypes.POINTER(ctypes.c_size_t)
        L.hivex_node_children.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        L.hivex_node_name.restype = ctypes.c_void_p
        L.hivex_node_name.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        kids = L.hivex_node_children(h.h, n)
        i = 0
        while kids[i]:
            p = L.hivex_node_name(h.h, kids[i])
            out.add(ctypes.string_at(p).decode("utf-8"))
            i += 1
    return out


# Microsoft's published KMS client setup keys ("Key Management Services (KMS) client activation and product keys").
# Only for the experimental non-LTSC editions, so their first start doesn't stop at a product-key page the user can't
# answer from the console. They pick the edition and do NOT activate Windows (that needs the user's own licence:
# product_key= in the answer file, or later in Windows). The tested IoT Enterprise LTSC flow uses no key.
SETUP_KEYS = {
    "Professional": "W269N-WFGWX-YVC9B-4J6C9-T83GX",
    "ProfessionalWorkstation": "NRG8B-VKK3Q-CXVCJ-9G2XF-6Q84J",
    "ProfessionalEducation": "6TP4R-GNPTD-KYYHQ-7B7DP-J447Y",
    "Education": "NW6C2-QMPVW-D7KKK-3GKT6-VCFB2",
    "Enterprise": "NPPR9-FWDCX-D2C8J-H872K-2YT43",
}


def settings(a, kv):
    s = {}
    s["computer_name"] = kv.get("computer_name", "PS5WIN")
    if not re.fullmatch(r"[A-Za-z0-9-]{1,15}", s["computer_name"]) or s["computer_name"].isdigit():
        raise Bad("computer_name: 1-15 letters, digits or '-', not only digits")
    s["user_name"] = kv.get("user_name", "ps5")
    if (not re.fullmatch(r"[^\"/\\\[\]:;|=,+*?<>@ ]{1,20}", s["user_name"]) or s["user_name"].endswith(".")
            or s["user_name"].lower() in RESERVED_USERS):
        raise Bad("user_name: 1-20 characters, none of \"/\\[]:;|=,+*?<>@ or space, not a built-in account name")
    s["password"] = kv.get("password", "")
    if len(s["password"]) > 127:
        raise Bad("password: at most 127 characters")
    lang = kv.get("locale", a.language)
    if not re.fullmatch(r"[a-z]{2,3}-[A-Z][A-Za-z]{1,3}(-[A-Za-z]+)?", lang):
        raise Bad(f"locale {lang!r} is not a language tag like en-US")
    s["locale"] = lang
    tz, why = kv.get("time_zone"), "answer file"
    if not tz:
        z = iana_zone(a.localtime)
        tz, why = (TZ[z], f"{z} from {a.localtime}") if z in TZ else ("UTC", f"{z or 'no zone'}: not in the table, UTC")
    s["time_zone"], s["time_zone_from"] = tz, why
    zones = image_zones(a.hive)
    if zones is not None and tz not in zones:
        raise Bad(f"time_zone {tz!r} is not one of the image's {len(zones)} time zones")
    pk = kv.get("product_key", "").upper()
    if pk and not re.fullmatch(r"([0-9A-Z]{5}-){4}[0-9A-Z]{5}", pk):
        raise Bad("product_key: five groups of five letters/digits")
    if pk == "XXXXX-XXXXX-XXXXX-XXXXX-XXXXX":
        raise Bad("product_key: still the example placeholder (put your own key, or leave the line commented out)")
    s["product_key"], s["key_kind"] = pk, "user" if pk else "none"
    if not pk and getattr(a, "edition_id", None) in SETUP_KEYS:      # experimental editions: no key prompt in OOBE
        s["product_key"], s["key_kind"] = SETUP_KEYS[a.edition_id], "setup"
    return s


def sub(parent, tag, text=None, **attrs):
    e = ET.SubElement(parent, f"{{{NS}}}{tag}", {k.replace("__", ":"): v for k, v in attrs.items()})
    if text is not None:
        e.text = text
    return e


def component(settings_el, name):
    return sub(settings_el, "component", name=name, **COMMON)


def secret(parent, value, suffix):
    p = sub(parent, "Password")
    if not value:                            # exactly what the 2026-11-17 install used (proven): empty, plain
        sub(p, "Value", "")
        sub(p, "PlainText", "true")
        return
    sub(p, "Value", base64.b64encode((value + suffix).encode("utf-16-le")).decode())
    sub(p, "PlainText", "false")


def build(s):
    ET.register_namespace("", NS)
    ET.register_namespace("wcm", WCM)
    root = ET.Element(f"{{{NS}}}unattend")
    spec = sub(root, "settings", **{"pass": "specialize"})
    sh = component(spec, "Microsoft-Windows-Shell-Setup")
    sub(sh, "ComputerName", s["computer_name"])
    sub(sh, "TimeZone", s["time_zone"])
    if s["product_key"]:
        sub(sh, "ProductKey", s["product_key"])
    oobe = sub(root, "settings", **{"pass": "oobeSystem"})
    intl = component(oobe, "Microsoft-Windows-International-Core")
    for tag in ("InputLocale", "SystemLocale", "UILanguage", "UserLocale"):
        sub(intl, tag, s["locale"])
    sh = component(oobe, "Microsoft-Windows-Shell-Setup")
    o = sub(sh, "OOBE")
    for tag in ("HideEULAPage", "HideLocalAccountScreen", "HideOnlineAccountScreens", "HideWirelessSetupInOOBE",
                "HideOEMRegistrationScreen"):
        sub(o, tag, "true")
    sub(o, "ProtectYourPC", "3")
    ua = sub(sh, "UserAccounts")
    la = sub(sub(ua, "LocalAccounts"), "LocalAccount", **{f"{{{WCM}}}action": "add"})
    secret(la, s["password"], "Password")
    sub(la, "Name", s["user_name"])
    sub(la, "Group", "Administrators")
    al = sub(sh, "AutoLogon")
    secret(al, s["password"], "Password")
    sub(al, "Enabled", "true")
    sub(al, "LogonCount", "9999")            # ps5-setup.cmd / firstlogon.cmd remove the count: autologon stays on
    sub(al, "Username", s["user_name"])
    # ps5-setup.cmd runs here because Windows skips SetupComplete.cmd on an install without a product key (OEM
    # channel: "will not run user-provided scripts", Toshiba run 3, 2026-11-21). SetupComplete stays for keyed
    # installs; C:\ps5\setup\done.txt makes the second call a no-op.
    flc = sub(sh, "FirstLogonCommands")
    for order, cmdline, desc in ((1, r"cmd /c C:\ps5\setup\ps5-setup.cmd", "PS5 setup (drivers, tasks, settings)"),
                                 (2, r"cmd /c C:\ps5\firstlogon.cmd", "PS5 first logon")):
        c = sub(flc, "SynchronousCommand", **{f"{{{WCM}}}action": "add"})
        sub(c, "Order", str(order))
        sub(c, "CommandLine", cmdline)
        sub(c, "Description", desc)
    ET.indent(root)
    return b'<?xml version="1.0" encoding="utf-8"?>\n' + ET.tostring(root, encoding="utf-8") + b"\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--answers")
    ap.add_argument("--edition-id", help="EDITIONID of the image being installed (picks a setup key, see SETUP_KEYS)")
    ap.add_argument("--language", default="en-US")
    ap.add_argument("--localtime", default="/etc/localtime")
    ap.add_argument("--hive")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    try:
        s = settings(a, read_answers(a.answers))
    except Bad as e:
        print(f"UNATTEND REFUSED: {e}")
        sys.exit(4)
    Path(a.out).write_bytes(build(s))
    shown = {k: v for k, v in s.items() if k not in ("password", "product_key")}
    shown["password"] = "set" if s["password"] else "none (empty, as today's install)"
    shown["product_key"] = {"user": "set", "setup": "edition setup key (not activated)"}.get(
        s["key_kind"], "none (not activated)")
    shown.pop("key_kind", None)
    if a.json:
        print(json.dumps(shown, indent=1))
    else:
        for k, v in shown.items():
            print(f"  {k}: {v}")
        print(f"unattend written: {a.out}")


if __name__ == "__main__":
    main()
