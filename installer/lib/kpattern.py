# SPDX-License-Identifier: MIT
"""Will the firmware's PATTERN stub patch this ntoskrnl.exe? The firmware uses it for a kernel that has no table stub
(firmware/windows_kstub_install.inc KpatInstall, since the r18 firmware). Same four checks, each "exactly once", on
the file as on disk (the firmware checks the loaded image again and stops with a red screen if any fails):
  entry     the PE entry point (KiSystemStartup) starts with the 12 known bytes the stub moves
  cave      exactly one POOLCODE section (code, not discardable) with >= 0x200 zero/int3 slack to its page end
  apic      the HAL's local-APIC read and write accessors exactly once each, naming the same variable
  halprobe  the HAL I/O APIC probe (44-byte FNV-1a fingerprint the firmware's HAL patch uses) exactly once
Same code as the dev launcher's check (build/boot44dev/run_stage.sh) and build/kpattern/kfind.py."""
import re
import struct

ENTRY = bytes.fromhex("4883ec384c897c24304c8bfc")
APIC_READ = re.compile(rb"\x48\x8b\x05(....)\x48\x63\xd1\x8b\x04\x02\xc3", re.S)
APIC_WRITE = re.compile(rb"\x48\x8b\x05(....)\x4c\x63\xc1\x41\x89\x14\x00\xc3", re.S)
PROBE_LEN, PROBE_FNV = 44, 0xb52d15455196d1b1


def _fnv(x):
    h = 0xcbf29ce484222325
    for c in x:
        h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def check(b):
    """Return the list of failed checks ([] = the pattern stub applies). Raises ValueError for a non-PE file."""
    e = struct.unpack_from("<I", b, 0x3C)[0]
    if b[e:e + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    ep = struct.unpack_from("<I", b, e + 40)[0]
    secs = []
    for i in range(struct.unpack_from("<H", b, e + 6)[0]):
        o = e + 24 + struct.unpack_from("<H", b, e + 20)[0] + 40 * i
        vs, va, rs, ro = struct.unpack_from("<IIII", b, o + 8)
        secs.append((b[o:o + 8].rstrip(b"\0"), va, vs, ro, rs, struct.unpack_from("<I", b, o + 36)[0]))

    def off(rva):
        return next((ro + rva - va for _, va, vs, ro, rs, _ in secs if va <= rva < va + max(vs, rs)), None)

    def rva(o):
        return next(va + o - ro for _, va, vs, ro, rs, _ in secs if ro <= o < ro + rs)

    why = []
    o = off(ep)
    if o is None or b[o:o + 12] != ENTRY:
        why.append("entry")
    cave = [b[ro + vs:ro + min(rs, (vs + 0xFFF) & ~0xFFF)] for n, va, vs, ro, rs, ch in secs
            if n == b"POOLCODE" and ch & 0x20000020 == 0x20000020 and not ch & 0x02000000
            and ((vs + 0xFFF) & ~0xFFF) - vs >= 0x200]
    if len(cave) != 1 or not set(cave[0]) <= {0, 0xCC}:
        why.append("cave")
    text = [(ro, rs) for n, va, vs, ro, rs, ch in secs if ch & 0x20000000]
    v = {}
    for name, rx in (("r", APIC_READ), ("w", APIC_WRITE)):
        v[name] = [rva(m.start()) + 7 + struct.unpack("<i", m.group(1))[0]
                   for ro, rs in text for m in rx.finditer(b, ro, ro + rs)]
    if len(v["r"]) != 1 or v["r"] != v["w"]:
        why.append("apic")
    probe = [i for ro, rs in text for i in range(ro, ro + rs - PROBE_LEN)
             if b[i] == 0xC7 and b[i + 1] == 0x03 and _fnv(b[i:i + PROBE_LEN]) == PROBE_FNV]
    if len(probe) != 1:
        why.append("halprobe")
    return why
