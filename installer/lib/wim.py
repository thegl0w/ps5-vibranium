"""WIM/ESD header + XML parser and the Windows edition pick, pure Python and read-only.

Works on any reader function read(offset, length) -> bytes, so the same code reads a WIM inside an ISO (via udf.py)
or a plain file on a USB stick. Only the 208-byte header and the uncompressed XML block are read.
"""
import struct
import xml.etree.ElementTree as ET

MAGIC = (b"MSWIM\0\0\0", b"WLPWM\0\0\0")
FLAG_SPANNED = 0x00000008
FLAG_COMPRESS = {0x00020000: "XPRESS", 0x00040000: "LZX", 0x00080000: "LZMS"}

# The release this setup was built and tested on: Windows 10 IoT Enterprise LTSC 2021 (21H2), x64. The kernel stub in
# the firmware is tied to the exact ntoskrnl build, so the build numbers are part of the identity.
TARGET = {"EDITIONID": "IoTEnterpriseS", "ARCH": "9", "MAJOR": "10", "MINOR": "0", "BUILD": "19041"}
KNOWN_SPBUILDS = {"1288": "LTSC 2021 RTM media (ntoskrnl stub: firmware/windows_kstub-19041.1288.inc)"}


class WimError(Exception):
    pass


def _reshdr(buf, off):
    raw = buf[off:off + 24]
    size = int.from_bytes(raw[0:7], "little")
    flags = raw[7]
    offset, orig = struct.unpack_from("<QQ", raw, 8)
    return {"size": size, "flags": flags, "offset": offset, "original_size": orig}


def read_header(read):
    h = read(0, 208)
    if h[:8] not in MAGIC:
        raise WimError("not a WIM/ESD file (bad magic)")
    hdr_size, version, flags, chunk = struct.unpack_from("<IIII", h, 8)
    part, total, count = struct.unpack_from("<HHI", h, 40)
    info = {
        "pipable": h[:8] == MAGIC[1],
        "version": version,
        "flags": flags,
        "compression": next((v for k, v in FLAG_COMPRESS.items() if flags & k), "none"),
        "chunk_size": chunk,
        "part": part,
        "total_parts": total,
        "image_count": count,
        "xml": _reshdr(h, 72),
        "boot_index": struct.unpack_from("<I", h, 120)[0],
    }
    if hdr_size != 208:
        raise WimError(f"unexpected header size {hdr_size}")
    return info


def read_images(read):
    info = read_header(read)
    x = info["xml"]
    if x["flags"] & 0x04:      # compressed resource: the XML is never compressed in Microsoft's media
        raise WimError("compressed XML block is not supported")
    if x["original_size"] > 16 * 1024 * 1024:
        raise WimError("XML block implausibly large")
    raw = read(x["offset"], x["original_size"])
    text = raw.decode("utf-16-le").lstrip("﻿")
    root = ET.fromstring(text)
    images = []
    for img in root.findall("IMAGE"):
        def g(path):
            e = img.find(path)
            return e.text.strip() if e is not None and e.text else ""
        images.append({
            "index": int(img.get("INDEX")),
            "name": g("NAME"),
            "description": g("DESCRIPTION"),
            "flags": g("FLAGS"),
            "EDITIONID": g("WINDOWS/EDITIONID"),
            "ARCH": g("WINDOWS/ARCH"),
            "INSTALLATIONTYPE": g("WINDOWS/INSTALLATIONTYPE"),
            "PRODUCTNAME": g("WINDOWS/PRODUCTNAME"),
            "MAJOR": g("WINDOWS/VERSION/MAJOR"),
            "MINOR": g("WINDOWS/VERSION/MINOR"),
            "BUILD": g("WINDOWS/VERSION/BUILD"),
            "SPBUILD": g("WINDOWS/VERSION/SPBUILD"),
            "LANGUAGES": [e.text for e in img.findall("WINDOWS/LANGUAGES/LANGUAGE") if e.text],
            "TOTALBYTES": int(g("TOTALBYTES") or 0),
        })
    return info, images


def pick_edition(info, images):
    """Return (image, verdict, reasons). verdict: 'ok' (exact tested release), 'untested' (right edition, other
    servicing build), or 'refuse'."""
    reasons = []
    if info["total_parts"] != 1:
        return None, "refuse", ["split WIM (.swm): copy the original ISO instead"]
    hits = [i for i in images if all(i.get(k) == v for k, v in TARGET.items())]
    if not hits:
        found = ", ".join(f"{i['index']}: {i['EDITIONID'] or i['name']} (arch {i['ARCH']}, build {i['BUILD']})"
                          for i in images) or "no images"
        return None, "refuse", [f"no Windows 10 IoT Enterprise LTSC x64 image in this file ({found})"]
    if len(hits) > 1:
        reasons.append(f"{len(hits)} matching images; taking the first (index {hits[0]['index']})")
    img = hits[0]
    if img["SPBUILD"] in KNOWN_SPBUILDS:
        reasons.append(f"build 10.0.{img['BUILD']}.{img['SPBUILD']}: {KNOWN_SPBUILDS[img['SPBUILD']]}")
        return img, "ok", reasons
    reasons.append(f"build 10.0.{img['BUILD']}.{img['SPBUILD']} has no kernel stub in the firmware yet: "
                   "Windows would boot without the per-CPU fix (see installer/DESIGN.md, kernel stub)")
    return img, "untested", reasons
