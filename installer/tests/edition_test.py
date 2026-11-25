#!/usr/bin/env python3
"""Offline tests: lib/wim.pick_edition (tested release vs experimental editions) and lib/kpattern (the firmware's
pattern-stub checks) on the kernel files kept in ../build when they are there. Reads only; writes nothing."""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE.parent), str(HERE.parent / "lib")]
import kpattern  # noqa: E402
import wim  # noqa: E402

INFO = {"total_parts": 1}
n = 0


def img(i, eid, name, sp="2965"):
    return {"index": i, "EDITIONID": eid, "name": name, "ARCH": "9", "MAJOR": "10", "MINOR": "0", "BUILD": "19041",
            "SPBUILD": sp}


ltsc = [img(1, "IoTEnterpriseS", "Windows 10 IoT Enterprise LTSC", "1288"), img(2, "EnterpriseS", "LTSC", "1288")]
consumer = [img(1, "Core", "Windows 10 Home"), img(2, "CoreN", "Home N"), img(4, "Education", "Windows 10 Education"),
            img(6, "Professional", "Windows 10 Pro"), img(7, "ProfessionalN", "Windows 10 Pro N"),
            img(10, "ProfessionalWorkstation", "Windows 10 Pro for Workstations")]

i, v, r = wim.pick_edition(INFO, ltsc)
assert (i["index"], v) == (1, "ok"), (i, v, r); n += 1
i, v, r = wim.pick_edition(INFO, [img(1, "IoTEnterpriseS", "IoT", "2965")])
assert v == "experimental", v; n += 1                       # right edition, other build: experimental now
i, v, r = wim.pick_edition(INFO, consumer)
assert (i["index"], v) == (6, "experimental"), (i, v); n += 1      # Pro before Workstation/Education
i, v, r = wim.pick_edition(INFO, consumer, "education")
assert (i["index"], v) == (4, "experimental"), (i, v); n += 1
i, v, r = wim.pick_edition(INFO, consumer, "Windows 10 Pro for Workstations")
assert i["index"] == 10, i; n += 1
for want in ("Core", "ProfessionalN", "Ultimate"):
    i, v, r = wim.pick_edition(INFO, consumer, want)
    assert (i, v) == (None, "refuse") and "not in this file" in r[-1], (want, v, r); n += 1
i, v, r = wim.pick_edition(INFO, [img(1, "Core", "Home"), img(2, "CoreN", "Home N")])
assert v == "refuse" and "Home and N" in r[-1], r; n += 1
i, v, r = wim.pick_edition(INFO, [dict(img(1, "Professional", "Pro"), ARCH="0")])
assert v == "refuse", v; n += 1                              # 32-bit
i, v, r = wim.pick_edition({"total_parts": 2}, consumer)
assert v == "refuse", v; n += 1

kernels = {"build/install-kernel/ntoskrnl.exe": "1288", "build/sym7725/ntoskrnl.exe": "7725",
           "build/22h2/ntoskrnl.exe": "2965"}
root = HERE.parent.parent
have = [k for k in kernels if (root / k).is_file()]
for k in have:
    assert kpattern.check((root / k).read_bytes()) == [], k; n += 1
if have:
    b = bytearray((root / have[-1]).read_bytes())
    e = struct.unpack_from("<I", b, 0x3C)[0]
    ep = struct.unpack_from("<I", b, e + 40)[0]
    for s in range(struct.unpack_from("<H", b, e + 6)[0]):
        o = e + 24 + struct.unpack_from("<H", b, e + 20)[0] + 40 * s
        vs, va, rs, ro = struct.unpack_from("<IIII", b, o + 8)
        if va <= ep < va + vs:
            b[ro + ep - va] ^= 0xFF
    assert kpattern.check(bytes(b)) == ["entry"]; n += 1
print(f"{n} edition/pattern cases passed ({len(have)} kernel files)")
