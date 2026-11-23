"""Minimal ctypes binding to libwim (wimlib 1.14) for the installer's one-pass apply.

The idea: open the user's install.wim READ-ONLY, add our files (C:\\ps5, unattend.xml, edited registry hives, the
SetupComplete script) to the chosen image IN MEMORY with wimlib_update_image, and extract that merged image in one
pass. The user's WIM is never written (no wimlib_write/overwrite call exists here), and the Windows partition needs no
second write pass after the apply (so no NTFS driver ever mounts it read-write).
"""
import ctypes
import ctypes.util
import os
import struct
import sys

UPDATE_OP_ADD, UPDATE_OP_DELETE = 0, 1
ADD_FLAG_NTFS, ADD_FLAG_VERBOSE = 0x1, 0x4
DELETE_FLAG_FORCE, DELETE_FLAG_RECURSIVE = 0x1, 0x2
EXTRACT_FLAG_NTFS = 0x1
EXTRACT_FLAG_NO_ACLS = 0x40
EXTRACT_FLAG_GLOB_PATHS = 0x40000
EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE = 0x200000


class _Add(ctypes.Structure):
    _fields_ = [("fs_source_path", ctypes.c_char_p), ("wim_target_path", ctypes.c_char_p),
                ("config_file", ctypes.c_char_p), ("add_flags", ctypes.c_int)]


class _Delete(ctypes.Structure):
    _fields_ = [("wim_path", ctypes.c_char_p), ("delete_flags", ctypes.c_int)]


class _U(ctypes.Union):
    _fields_ = [("add", _Add), ("delete", _Delete)]


class UpdateCommand(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("op", ctypes.c_int), ("u", _U)]


def _load():
    for cand in (os.environ.get("LIBWIM"), "/opt/homebrew/lib/libwim.dylib", ctypes.util.find_library("wim"),
                 "libwim.so.15"):
        if cand:
            try:
                return ctypes.CDLL(cand)
            except OSError:
                pass
    sys.exit("libwim (wimlib) not found")


L = _load()
L.wimlib_get_error_string.restype = ctypes.c_char_p
L.wimlib_open_wim.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p)]
L.wimlib_update_image.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(UpdateCommand), ctypes.c_size_t,
                                  ctypes.c_int]
L.wimlib_extract_image.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
L.wimlib_extract_paths.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p,
                                   ctypes.POINTER(ctypes.c_char_p), ctypes.c_size_t, ctypes.c_int]
L.wimlib_free.argtypes = [ctypes.c_void_p]
PROGRESS_FUNC = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)
L.wimlib_register_progress_function.argtypes = [ctypes.c_void_p, PROGRESS_FUNC, ctypes.c_void_p]
MSG_EXTRACT_STREAMS = 4
L.wimlib_global_init(0)


class WimlibError(Exception):
    pass


def _ok(rc, what):
    if rc != 0:
        raise WimlibError(f"{what}: {L.wimlib_get_error_string(rc).decode()} (code {rc})")


class Wim:
    """A WIM opened read-only (open flags 0: no write access is ever requested)."""

    def __init__(self, path):
        self.h = ctypes.c_void_p()
        _ok(L.wimlib_open_wim(os.fsencode(path), 0, ctypes.byref(self.h)), f"open {path}")

    def close(self):
        if self.h:
            L.wimlib_free(self.h)
            self.h = ctypes.c_void_p()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def overlay(self, index, adds=(), deletes=()):
        """adds: [(local file or dir, path in image)], replacing existing files; deletes: [path in image]."""
        keep = []
        cmds = (UpdateCommand * (len(adds) + len(deletes)))()
        for i, (src, dst) in enumerate(adds):
            s, d = os.fsencode(src), dst.encode("utf-8")
            keep += [s, d]
            cmds[i].op = UPDATE_OP_ADD
            cmds[i].add.fs_source_path, cmds[i].add.wim_target_path = s, d
            cmds[i].add.config_file, cmds[i].add.add_flags = None, 0
        for j, path in enumerate(deletes, len(adds)):
            p = path.encode("utf-8")
            keep.append(p)
            cmds[j].op = UPDATE_OP_DELETE
            cmds[j].delete.wim_path, cmds[j].delete.delete_flags = p, DELETE_FLAG_FORCE | DELETE_FLAG_RECURSIVE
        if len(cmds):
            _ok(L.wimlib_update_image(self.h, index, cmds, len(cmds), 0), "update image (in memory)")

    def extract_image(self, index, target, flags=0, progress=None):
        """progress(done_bytes, total_bytes) is called while file data is written (wimlib's EXTRACT_STREAMS)."""
        if progress:
            def cb(msg, info, ctx):
                if msg == MSG_EXTRACT_STREAMS and info:
                    total, done = struct.unpack_from("<QQ", ctypes.string_at(info, 56), 40)
                    progress(done, total)
                return 0
            self._cb = PROGRESS_FUNC(cb)          # keep a reference while wimlib may call it
            L.wimlib_register_progress_function(self.h, self._cb, None)
        _ok(L.wimlib_extract_image(self.h, index, os.fsencode(target), flags), f"extract image {index}")

    def extract_paths(self, index, target, paths, flags=0):
        arr = (ctypes.c_char_p * len(paths))(*[p.encode("utf-8") for p in paths])
        _ok(L.wimlib_extract_paths(self.h, index, os.fsencode(target), arr, len(paths), flags), "extract paths")
