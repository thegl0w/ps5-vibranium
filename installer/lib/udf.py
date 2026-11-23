"""Read-only reader for the UDF file system on Windows install ISOs (no mount, no loop device, pure Python).

Microsoft ISOs carry the real file tree only in UDF (the ISO 9660 part is a stub), so this walks UDF directly:
anchor (sector 256) -> volume descriptors -> file set -> root directory -> file entries. Enough to list files, find
sources/install.wim and read byte ranges of it (the WIM header and XML) straight from the ISO file.
"""
import struct

SECTOR = 2048


class UdfError(Exception):
    pass


def _tag(buf, want=None):
    ident = struct.unpack_from("<H", buf, 0)[0]
    if want is not None and ident != want:
        raise UdfError(f"expected descriptor tag {want}, found {ident}")
    # tag checksum: sum of bytes 0-3 and 5-15, mod 256, must equal byte 4
    if (sum(buf[0:4]) + sum(buf[5:16])) & 0xFF != buf[4]:
        raise UdfError(f"bad tag checksum on descriptor {ident}")
    return ident


def _dstring(raw):
    if not raw:
        return ""
    comp = raw[0]
    if comp == 8:
        return raw[1:].decode("latin-1")
    if comp == 16:
        return raw[1:].decode("utf-16-be")
    raise UdfError(f"unknown dstring compression {comp}")


class UdfImage:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.part_start = None
        self.block = SECTOR
        self.label = ""
        self._mount()

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def _read(self, offset, length):
        self.f.seek(offset)
        data = self.f.read(length)
        if len(data) != length:
            raise UdfError("short read (truncated image?)")
        return data

    def _sector(self, lba, count=1):
        return self._read(lba * SECTOR, count * SECTOR)

    def _mount(self):
        avdp = self._sector(256)
        _tag(avdp, 2)
        vds_len, vds_loc = struct.unpack_from("<II", avdp, 16)
        fsd_ad = None
        for i in range(vds_len // SECTOR):
            d = self._sector(vds_loc + i)
            ident = _tag(d)
            if ident == 5:      # partition descriptor
                self.part_start = struct.unpack_from("<I", d, 188)[0]
            elif ident == 6:    # logical volume descriptor
                self.block = struct.unpack_from("<I", d, 212)[0]
                self.label = _dstring(d[84:84 + d[84 + 127]]) if d[84 + 127] else ""
                fsd_ad = struct.unpack_from("<IIH", d, 248)
            elif ident == 8:    # terminating descriptor
                break
        if self.part_start is None or fsd_ad is None:
            raise UdfError("no partition / logical volume descriptor")
        if self.block != SECTOR:
            raise UdfError(f"unsupported logical block size {self.block}")
        fsd = self._block(fsd_ad[1])
        _tag(fsd, 256)
        root_len, root_lbn = struct.unpack_from("<II", fsd, 400)
        self.root = self._entry(root_lbn)

    def _block(self, lbn, count=1):
        return self._sector(self.part_start + lbn, count)

    def _entry(self, lbn):
        """Parse a (extended) file entry -> dict(type, size, extents[(byte offset in image, length)], inline)."""
        d = self._block(lbn)
        ident = _tag(d)
        if ident == 261:
            size = struct.unpack_from("<Q", d, 56)[0]
            l_ea, l_ad = struct.unpack_from("<II", d, 168)
            ad_off = 176 + l_ea
        elif ident == 266:
            size = struct.unpack_from("<Q", d, 56)[0]
            l_ea, l_ad = struct.unpack_from("<II", d, 208)
            ad_off = 216 + l_ea
        else:
            raise UdfError(f"descriptor {ident} at block {lbn} is not a file entry")
        ftype = d[16 + 11]
        adtype = struct.unpack_from("<H", d, 16 + 18)[0] & 7
        extents, inline = [], None
        if adtype == 3:
            inline = d[ad_off:ad_off + l_ad]
        else:
            step = {0: 8, 1: 16}.get(adtype)
            if step is None:
                raise UdfError(f"unsupported allocation descriptor type {adtype}")
            for o in range(ad_off, ad_off + l_ad, step):
                ln, pos = struct.unpack_from("<II", d, o)
                kind, ln = ln >> 30, ln & 0x3FFFFFFF
                if ln == 0:
                    break
                if kind == 3:
                    raise UdfError("continued allocation descriptors are not supported")
                if kind == 0:   # recorded and allocated
                    extents.append(((self.part_start + pos) * SECTOR, ln))
        return {"type": ftype, "size": size, "extents": extents, "inline": inline}

    def read_entry(self, ent, offset=0, length=None):
        if length is None:
            length = ent["size"] - offset
        length = max(0, min(length, ent["size"] - offset))
        if ent["inline"] is not None:
            return ent["inline"][offset:offset + length]
        out, pos = bytearray(), 0
        for start, ln in ent["extents"]:
            if length <= 0:
                break
            if offset < pos + ln:
                skip = max(0, offset - pos)
                take = min(ln - skip, length)
                out += self._read(start + skip, take)
                offset += take
                length -= take
            pos += ln
        return bytes(out)

    def extract(self, ent, dest, chunk=8 << 20, progress=None):
        """Copy a file out of the image (streamed, extent by extent). Returns the number of bytes written."""
        done = 0
        with open(dest, "wb") as out:
            if ent["inline"] is not None:
                out.write(ent["inline"][:ent["size"]])
                return ent["size"]
            for start, ln in ent["extents"]:
                ln = min(ln, ent["size"] - done)
                pos = 0
                while pos < ln:
                    n = min(chunk, ln - pos)
                    out.write(self._read(start + pos, n))
                    pos += n
                    done += n
                    if progress:
                        progress(done, ent["size"])
        if done != ent["size"]:
            raise UdfError(f"extracted {done} of {ent['size']} bytes")
        return done

    def listdir(self, ent):
        if ent["type"] != 4:
            raise UdfError("not a directory")
        data = self.read_entry(ent)
        o, out = 0, []
        while o + 38 <= len(data):
            _tag(data[o:o + 16], 257)
            chars = data[o + 18]
            l_fi = data[o + 19]
            icb_lbn = struct.unpack_from("<I", data, o + 24)[0]
            l_iu = struct.unpack_from("<H", data, o + 36)[0]
            name = _dstring(data[o + 38 + l_iu:o + 38 + l_iu + l_fi])
            if not chars & 0x08 and not chars & 0x04:   # skip parent and deleted entries
                out.append((name, icb_lbn, bool(chars & 0x02)))
            o += (38 + l_iu + l_fi + 3) & ~3
        return out

    def lookup(self, path):
        ent = self.root
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            for name, lbn, _ in self.listdir(ent):
                if name.lower() == part.lower():
                    ent = self._entry(lbn)
                    break
            else:
                return None
        return ent


def iso9660_label(path):
    """Volume label from the ISO 9660 primary volume descriptor (sector 16), or '' if absent."""
    with open(path, "rb") as f:
        f.seek(16 * SECTOR)
        pvd = f.read(SECTOR)
    if len(pvd) == SECTOR and pvd[0] == 1 and pvd[1:6] == b"CD001":
        return pvd[40:72].decode("ascii", "replace").strip()
    return ""
