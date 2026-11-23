"""Read a Windows security catalog (.cat) offline: its member entries (tag + attributes), from the DER bytes.

A .cat is a PKCS#7 SignedData whose content is a Certificate Trust List (OID 1.3.6.1.4.1.311.10.1): a SEQUENCE with
subjectUsage, listIdentifier, thisUpdate, subjectAlgorithm and then trustedSubjects = SEQUENCE OF { subjectIdentifier
OCTET STRING (the member tag), attributes SET }. Windows finds a member by binary search on the tag, so the entries
must be in ascending tag order (a hash-swapped catalog kept the old order and 19044.7725 rejected it, 2026-11-22).
"""

CTL_OID = "1.3.6.1.4.1.311.10.1"


class CatError(Exception):
    pass


def _tlv(b, o):
    """(tag, header length, content length) of the DER element at o."""
    if o + 2 > len(b):
        raise CatError("truncated")
    tag = b[o]
    ln = b[o + 1]
    hl = 2
    if ln & 0x80:
        n = ln & 0x7F
        if n == 0 or n > 4 or o + 2 + n > len(b):
            raise CatError("bad length")
        ln = int.from_bytes(b[o + 2:o + 2 + n], "big")
        hl += n
    if o + hl + ln > len(b):
        raise CatError("element runs past the end")
    return tag, hl, ln


def _children(b, o):
    tag, hl, ln = _tlv(b, o)
    out, p, end = [], o + hl, o + hl + ln
    while p < end:
        t, h, l = _tlv(b, p)
        out.append((t, p, h, l))
        p += h + l
    return out


def _oid(raw):
    first = raw[0]
    parts, v = [first // 40, first % 40], 0
    for x in raw[1:]:
        v = (v << 7) | (x & 0x7F)
        if not x & 0x80:
            parts.append(v)
            v = 0
    return ".".join(map(str, parts))


def members(data):
    """List of (tag bytes, [attribute OIDs]) in file order."""
    ci = _children(data, 0)                         # ContentInfo: contentType, [0] content
    if len(ci) < 2:
        raise CatError("not a PKCS#7 ContentInfo")
    sd = _children(data, ci[1][1])[0]                # [0] -> SignedData SEQUENCE
    sdk = _children(data, sd[1])
    enc = next(c for c in sdk if c[0] == 0x30 and _children(data, c[1])
               and _children(data, c[1])[0][0] == 0x06)        # encapContentInfo
    ek = _children(data, enc[1])
    t, p, h, l = ek[0]
    if _oid(data[p + h:p + h + l]) != CTL_OID:
        raise CatError("content is not a certificate trust list")
    ctl = _children(data, ek[1][1])[0]               # [0] -> CTL SEQUENCE
    subjects = None
    for t, p, h, l in _children(data, ctl[1]):
        if t == 0x30:
            kids = _children(data, p)
            if kids and all(k[0] == 0x30 for k in kids) and _children(data, kids[0][1]) and \
                    _children(data, kids[0][1])[0][0] == 0x04:
                subjects = (t, p, h, l)
    if subjects is None:
        return []
    out = []
    for t, p, h, l in _children(data, subjects[1]):
        kids = _children(data, p)
        tt, tp, th, tl = kids[0]
        tag = data[tp + th:tp + th + tl]
        oids = []
        if len(kids) > 1:
            for at in _children(data, kids[1][1]):
                ak = _children(data, at[1])
                if ak and ak[0][0] == 0x06:
                    oids.append(_oid(data[ak[0][1] + ak[0][2]:ak[0][1] + ak[0][2] + ak[0][3]]))
        out.append((tag, oids))
    return out


ALG_BY_LEN = {20: "sha1", 32: "sha256"}


def tag_text(tag):
    """Member tags in our catalogs are the raw digests (20 bytes SHA-1, 32 bytes SHA-256): shown as hex."""
    return tag.hex().upper()


def unsorted(data):
    """Positions where the member order breaks ascending tag order (byte-wise; empty = sorted). Every catalog Windows
    made for us (makecat on the console, New-FileCatalog) is in this order."""
    tags = [t for t, _ in members(data)]
    return [i for i in range(1, len(tags)) if tags[i - 1] > tags[i]]
