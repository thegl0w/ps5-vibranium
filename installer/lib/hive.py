"""Per-value offline registry editing through libhivex (ctypes).

Why not hivexsh: its `setval` REPLACES ALL VALUES of a key, which is fine for brand-new keys (winpe/prepare.py uses it
that way) but would wipe e.g. ControlSet001\\Control. hivex_node_set_value changes one value and keeps the rest.
Only ever used on COPIES of hives extracted from install.wim; the edited copy goes into the image via the overlay.
"""
import ctypes
import ctypes.util
import os
import struct
import sys

REG_SZ, REG_EXPAND_SZ, REG_BINARY, REG_DWORD, REG_MULTI_SZ = 1, 2, 3, 4, 7
OPEN_WRITE = 4


class _SetValue(ctypes.Structure):
    _fields_ = [("key", ctypes.c_char_p), ("t", ctypes.c_int), ("len", ctypes.c_size_t),
                ("value", ctypes.c_char_p)]


def _load():
    for cand in (os.environ.get("LIBHIVEX"), "/opt/homebrew/lib/libhivex.dylib", ctypes.util.find_library("hivex"),
                 "libhivex.so.0"):
        if cand:
            try:
                return ctypes.CDLL(cand, use_errno=True)
            except OSError:
                pass
    sys.exit("libhivex not found")


L = _load()
N = ctypes.c_size_t                       # hive_node_h / hive_value_h are size_t
L.hivex_open.restype = ctypes.c_void_p
L.hivex_open.argtypes = [ctypes.c_char_p, ctypes.c_int]
L.hivex_root.restype = N
L.hivex_root.argtypes = [ctypes.c_void_p]
L.hivex_node_get_child.restype = N
L.hivex_node_get_child.argtypes = [ctypes.c_void_p, N, ctypes.c_char_p]
L.hivex_node_add_child.restype = N
L.hivex_node_add_child.argtypes = [ctypes.c_void_p, N, ctypes.c_char_p]
L.hivex_node_get_value.restype = N
L.hivex_node_get_value.argtypes = [ctypes.c_void_p, N, ctypes.c_char_p]
L.hivex_value_value.restype = ctypes.POINTER(ctypes.c_char)
L.hivex_value_value.argtypes = [ctypes.c_void_p, N, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_size_t)]
L.hivex_node_set_value.argtypes = [ctypes.c_void_p, N, ctypes.POINTER(_SetValue), ctypes.c_int]
L.hivex_commit.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
L.hivex_close.argtypes = [ctypes.c_void_p]
libc = ctypes.CDLL(None)
libc.free.argtypes = [ctypes.c_void_p]


class HiveError(Exception):
    pass


def encode(t, v):
    if t == REG_DWORD:
        return struct.pack("<I", v)
    if t in (REG_SZ, REG_EXPAND_SZ):
        return (v + "\0").encode("utf-16-le")
    if t == REG_MULTI_SZ:
        return ("".join(s + "\0" for s in v) + "\0").encode("utf-16-le")
    if t == REG_BINARY:
        return bytes(v)
    raise HiveError(f"type {t} not supported")


def decode(t, raw):
    if t == REG_DWORD:
        return struct.unpack("<I", raw[:4])[0]
    if t in (REG_SZ, REG_EXPAND_SZ):
        return raw.decode("utf-16-le").split("\0")[0]
    if t == REG_MULTI_SZ:
        return [s for s in raw.decode("utf-16-le").split("\0") if s]
    return raw


class Hive:
    def __init__(self, path, write=False):
        self.path = path
        self.h = L.hivex_open(os.fsencode(path), OPEN_WRITE if write else 0)
        if not self.h:
            raise HiveError(f"cannot open hive {path} (errno {ctypes.get_errno()})")

    def close(self):
        if self.h:
            L.hivex_close(self.h)
            self.h = None

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def node(self, path, create=False):
        n = L.hivex_root(self.h)
        for part in [p for p in path.split("\\") if p]:
            c = L.hivex_node_get_child(self.h, n, part.encode("utf-8"))
            if not c:
                if not create:
                    return 0
                c = L.hivex_node_add_child(self.h, n, part.encode("utf-8"))
                if not c:
                    raise HiveError(f"cannot create key {part} in {path}")
            n = c
        return n

    def get(self, path, name):
        n = self.node(path)
        if not n:
            return None
        v = L.hivex_node_get_value(self.h, n, name.encode("utf-8"))
        if not v:
            return None
        t, ln = ctypes.c_int(), ctypes.c_size_t()
        p = L.hivex_value_value(self.h, v, ctypes.byref(t), ctypes.byref(ln))
        if not p:
            raise HiveError(f"cannot read {path}\\{name}")
        raw = ctypes.string_at(p, ln.value)
        libc.free(ctypes.cast(p, ctypes.c_void_p))
        return t.value, decode(t.value, raw)

    def set(self, path, name, t, value):
        n = self.node(path, create=True)
        data = encode(t, value)
        sv = _SetValue(name.encode("utf-8"), t, len(data), data)
        if L.hivex_node_set_value(self.h, n, ctypes.byref(sv), 0) != 0:
            raise HiveError(f"cannot set {path}\\{name} (errno {ctypes.get_errno()})")

    def commit(self):
        if L.hivex_commit(self.h, None, 0) != 0:
            raise HiveError(f"commit failed (errno {ctypes.get_errno()})")


# ---- tree walking (BCD reading/compare) ---------------------------------------------------------------------------
L.hivex_node_children.restype = ctypes.POINTER(ctypes.c_size_t)
L.hivex_node_children.argtypes = [ctypes.c_void_p, N]
L.hivex_node_name.restype = ctypes.c_void_p
L.hivex_node_name.argtypes = [ctypes.c_void_p, N]
L.hivex_node_values.restype = ctypes.POINTER(ctypes.c_size_t)
L.hivex_node_values.argtypes = [ctypes.c_void_p, N]
L.hivex_value_key.restype = ctypes.c_void_p
L.hivex_value_key.argtypes = [ctypes.c_void_p, N]


def _take_str(p):
    s = ctypes.string_at(p).decode("utf-8", "replace")
    libc.free(ctypes.c_void_p(p))
    return s


def _list(p):
    out, i = [], 0
    while p[i]:
        out.append(p[i])
        i += 1
    libc.free(ctypes.cast(p, ctypes.c_void_p))
    return out


def children(h, node):
    return [(_take_str(L.hivex_node_name(h.h, c)), c) for c in _list(L.hivex_node_children(h.h, node))]


def values(h, node):
    out = []
    for v in _list(L.hivex_node_values(h.h, node)):
        name = _take_str(L.hivex_value_key(h.h, v))
        t, ln = ctypes.c_int(), ctypes.c_size_t()
        p = L.hivex_value_value(h.h, v, ctypes.byref(t), ctypes.byref(ln))
        raw = ctypes.string_at(p, ln.value)
        libc.free(ctypes.cast(p, ctypes.c_void_p))
        out.append((name, t.value, raw))
    return out


def walk(h, node=None, path=""):
    """Yield (path, [(name, type, raw bytes)]) for every key below node (root by default), sorted by name."""
    node = node or L.hivex_root(h.h)
    yield path, values(h, node)
    for name, c in sorted(children(h, node), key=lambda x: x[0].lower()):
        yield from walk(h, c, f"{path}\\{name}" if path else name)
