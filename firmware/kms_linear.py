#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""ps5win copy of ps5-hackintosh bringup/tools/kms_linear.py (MIT, rodri0x) with this project's link check, black screen and
refresh-rate choice (MIT, ps5win contributors).
Put a LINEAR XR24 (the screen's own resolution; was 2560x1440) dumb buffer on the screen with a recognisable test pattern, then hold the DRM fd.

Run as root after stopping the compositor (sway). Leaves the process sleeping so the framebuffer stays scanned out
until the machine crashes into the next stage. The first 8 pixels of row 0 are a signature (0xDEADBE00+i) so the
buffer can be located in VRAM later.
"""
import ctypes, fcntl, mmap, os, re, struct, sys, time

W, H = 2560, 1440

def _IOC(d, nr, size): return (d << 30) | (size << 16) | (0x64 << 8) | nr
def IOWR(nr, size): return _IOC(3, nr, size)

class ModeInfo(ctypes.Structure):
    _fields_ = [("clock", ctypes.c_uint32), ("hdisplay", ctypes.c_uint16), ("hsync_start", ctypes.c_uint16),
                ("hsync_end", ctypes.c_uint16), ("htotal", ctypes.c_uint16), ("hskew", ctypes.c_uint16),
                ("vdisplay", ctypes.c_uint16), ("vsync_start", ctypes.c_uint16), ("vsync_end", ctypes.c_uint16),
                ("vtotal", ctypes.c_uint16), ("vscan", ctypes.c_uint16), ("vrefresh", ctypes.c_uint32),
                ("flags", ctypes.c_uint32), ("type", ctypes.c_uint32), ("name", ctypes.c_char * 32)]

class CardRes(ctypes.Structure):
    _fields_ = [("fb_id_ptr", ctypes.c_uint64), ("crtc_id_ptr", ctypes.c_uint64),
                ("connector_id_ptr", ctypes.c_uint64), ("encoder_id_ptr", ctypes.c_uint64),
                ("count_fbs", ctypes.c_uint32), ("count_crtcs", ctypes.c_uint32),
                ("count_connectors", ctypes.c_uint32), ("count_encoders", ctypes.c_uint32),
                ("min_width", ctypes.c_uint32), ("max_width", ctypes.c_uint32),
                ("min_height", ctypes.c_uint32), ("max_height", ctypes.c_uint32)]

class GetConnector(ctypes.Structure):
    _fields_ = [("encoders_ptr", ctypes.c_uint64), ("modes_ptr", ctypes.c_uint64),
                ("props_ptr", ctypes.c_uint64), ("prop_values_ptr", ctypes.c_uint64),
                ("count_modes", ctypes.c_uint32), ("count_props", ctypes.c_uint32),
                ("count_encoders", ctypes.c_uint32), ("encoder_id", ctypes.c_uint32),
                ("connector_id", ctypes.c_uint32), ("connector_type", ctypes.c_uint32),
                ("connector_type_id", ctypes.c_uint32), ("connection", ctypes.c_uint32),
                ("mm_width", ctypes.c_uint32), ("mm_height", ctypes.c_uint32),
                ("subpixel", ctypes.c_uint32), ("pad", ctypes.c_uint32)]

class GetEncoder(ctypes.Structure):
    _fields_ = [("encoder_id", ctypes.c_uint32), ("encoder_type", ctypes.c_uint32),
                ("crtc_id", ctypes.c_uint32), ("possible_crtcs", ctypes.c_uint32),
                ("possible_clones", ctypes.c_uint32)]

class CreateDumb(ctypes.Structure):
    _fields_ = [("height", ctypes.c_uint32), ("width", ctypes.c_uint32), ("bpp", ctypes.c_uint32),
                ("flags", ctypes.c_uint32), ("handle", ctypes.c_uint32), ("pitch", ctypes.c_uint32),
                ("size", ctypes.c_uint64)]

class MapDumb(ctypes.Structure):
    _fields_ = [("handle", ctypes.c_uint32), ("pad", ctypes.c_uint32), ("offset", ctypes.c_uint64)]

class FbCmd(ctypes.Structure):
    _fields_ = [("fb_id", ctypes.c_uint32), ("width", ctypes.c_uint32), ("height", ctypes.c_uint32),
                ("pitch", ctypes.c_uint32), ("bpp", ctypes.c_uint32), ("depth", ctypes.c_uint32),
                ("handle", ctypes.c_uint32)]

class SetCrtc(ctypes.Structure):
    _fields_ = [("set_connectors_ptr", ctypes.c_uint64), ("count_connectors", ctypes.c_uint32),
                ("crtc_id", ctypes.c_uint32), ("fb_id", ctypes.c_uint32), ("x", ctypes.c_uint32),
                ("y", ctypes.c_uint32), ("gamma_size", ctypes.c_uint32), ("mode_valid", ctypes.c_uint32),
                ("mode", ModeInfo)]

GETRESOURCES = IOWR(0xA0, ctypes.sizeof(CardRes))
SETCRTC = IOWR(0xA2, ctypes.sizeof(SetCrtc))
GETENCODER = IOWR(0xA6, ctypes.sizeof(GetEncoder))
GETCONNECTOR = IOWR(0xA7, ctypes.sizeof(GetConnector))
ADDFB = IOWR(0xAE, ctypes.sizeof(FbCmd))
CREATE_DUMB = IOWR(0xB2, ctypes.sizeof(CreateDumb))
MAP_DUMB = IOWR(0xB3, ctypes.sizeof(MapDumb))
SET_MASTER = _IOC(0, 0x1E, 0)

def ioctl(fd, req, obj):
    fcntl.ioctl(fd, req, obj, True)

def u32s(n): return (ctypes.c_uint32 * max(n, 1))()

def carve_from_iomem():
    """(base, size) of the VRAM carve-out: the loader grows it downward from the fixed top 0x470000000 (vram.txt), and /proc/iomem
    lists it as a top-level 'Reserved' range ending at 0x46fffffff. 512 MB at 0x450000000 if that is not found."""
    try:
        for line in open("/proc/iomem"):
            m = re.match(r"^([0-9a-f]+)-([0-9a-f]+) : Reserved", line)
            if m and int(m.group(2), 16) == 0x46FFFFFFF:
                base = int(m.group(1), 16)
                return base, 0x470000000 - base
    except OSError:
        pass
    return 0x450000000, 0x20000000

CARVE, CARVE_SIZE = carve_from_iomem()
LOW_LIMIT = 0x0F000000

def find_vram(memfd, sig, hint, below=None):
    """VRAM offset of the buffer that starts with sig (the carve-out is system RAM): try the predicted spot, then scan down
    from just below the previous buffer (any picture size), then the rest from the top."""
    if hint is not None and hint >= 0 and os.pread(memfd, 32, CARVE + hint) == sig:
        return hint
    top = CARVE_SIZE - 0x1000 if below is None else below - 0x1000
    for off in range(top, -1, -0x1000):
        if os.pread(memfd, 32, CARVE + off) == sig:
            return off
    if below is not None:
        for off in range(CARVE_SIZE - 0x1000, top, -0x1000):
            if os.pread(memfd, 32, CARVE + off) == sig:
                return off
    return None

def link_state():
    """Display link registers, read in kernel mode (read() on /dev/mem; a user mapping of the GPU's BAR is filtered on
    this console). Read-only. Returns (trained, text). Dword offsets from dcn_2_0_1_offset.h on the DMU bases."""
    try:
        bar5 = int(open("/sys/bus/pci/devices/0000:20:00.0/resource").read().split("\n")[5].split()[0], 16)
        fd = os.open("/dev/mem", os.O_RDONLY)
        rd = lambda r: int.from_bytes(os.pread(fd, 4, bar5 + r * 4), "little")
        link, be_en, fe, vid = rd(0x55C8), rd(0x5570), rd(0x5528), rd(0x55CC)
        os.close(fd)
    except Exception as e:
        return True, f"not readable ({e}); not checked"
    text = f"DP_LINK_CNTL {link:#x} DIG_BE_EN {be_en:#x} DIG_FE {fe:#x} VID_STREAM {vid:#x}"
    return bool(link & 0x10), text

def main():
    global W, H
    fd = os.open("/dev/dri/card0", os.O_RDWR)
    try:
        fcntl.ioctl(fd, SET_MASTER)
    except OSError as e:
        print("SET_MASTER failed (is sway still running?):", e); sys.exit(1)

    res = CardRes()
    ioctl(fd, GETRESOURCES, res)
    crtcs, conns, encs, fbs = u32s(res.count_crtcs), u32s(res.count_connectors), u32s(res.count_encoders), u32s(res.count_fbs)
    res.crtc_id_ptr, res.connector_id_ptr = ctypes.addressof(crtcs), ctypes.addressof(conns)
    res.encoder_id_ptr, res.fb_id_ptr = ctypes.addressof(encs), ctypes.addressof(fbs)
    ioctl(fd, GETRESOURCES, res)
    print("crtcs", list(crtcs)[:res.count_crtcs], "connectors", list(conns)[:res.count_connectors])

    chosen = None
    fallback_mode = None
    for i in range(res.count_connectors):
        c = GetConnector(); c.connector_id = conns[i]
        ioctl(fd, GETCONNECTOR, c)
        if c.connection != 1 or c.count_modes == 0:
            continue
        modes = (ModeInfo * c.count_modes)(); encl = u32s(c.count_encoders)
        props, pvals = u32s(c.count_props), (ctypes.c_uint64 * max(c.count_props, 1))()
        c.modes_ptr, c.encoders_ptr = ctypes.addressof(modes), ctypes.addressof(encl)
        c.props_ptr, c.prop_values_ptr = ctypes.addressof(props), ctypes.addressof(pvals)
        ioctl(fd, GETCONNECTOR, c)
        for m in modes:
            print(f"  connector {c.connector_id} mode {m.hdisplay}x{m.vdisplay}@{m.vrefresh} type={m.type:#x}")
        # ps5win: the screen's own resolution: PS5WIN_MODE=<w>x<h> if the screen offers it, else its preferred mode's
        # resolution (largest if none is marked), capped at 3840x2160. At that resolution, the highest refresh rate up to
        # PS5WIN_MAX_HZ (default 120; 60 forces 60 Hz), preferring the "preferred" mode on a tie. The mode nearest 60 Hz is
        # kept as the fallback if the faster link does not train (below).
        max_hz = int(os.environ.get("PS5WIN_MAX_HZ", "120") or 120)
        usable = [m for m in modes if m.hdisplay <= 3840 and m.vdisplay <= 2160 and m.hdisplay % 8 == 0]
        if not usable:
            continue
        want = os.environ.get("PS5WIN_MODE", "")
        mw = re.match(r"^(\d+)x(\d+)$", want)
        if mw and any(m.hdisplay == int(mw.group(1)) and m.vdisplay == int(mw.group(2)) for m in usable):
            tw, th = int(mw.group(1)), int(mw.group(2))
        else:
            pref = [m for m in usable if m.type & 0x8]
            top = max(pref or usable, key=lambda m: (m.hdisplay * m.vdisplay, m.vrefresh))
            tw, th = top.hdisplay, top.vdisplay
        fits = [m for m in usable if m.hdisplay == tw and m.vdisplay == th]
        under = [m for m in fits if m.vrefresh <= max_hz] or fits
        best = max(under, key=lambda m: (m.vrefresh, m.type & 0x8))
        slow = min(fits, key=lambda m: (abs(m.vrefresh - 60), m.vrefresh))
        chosen = (c, best)
        fallback_mode = slow if slow.vrefresh < best.vrefresh else None
        W, H = tw, th
        print(f"chosen mode {best.hdisplay}x{best.vdisplay}@{best.vrefresh} (max {max_hz} Hz"
              + (f", asked {want}" if want else "") + ")" + (f", fallback @{slow.vrefresh}" if fallback_mode else ""))
        break
    if not chosen:
        print("no connected connector with a usable mode"); sys.exit(1)
    c, mode = chosen
    e = GetEncoder(); e.encoder_id = c.encoder_id
    ioctl(fd, GETENCODER, e)
    crtc_id = e.crtc_id or crtcs[0]
    print("using connector", c.connector_id, "crtc", crtc_id)

    # The GPU's BAR0 (256 MB at 0xd0000000) is an alias of the first 256 MB of the VRAM carve-out: macOS' IONDRVFramebuffer only
    # accepts a boot framebuffer inside a PCI BAR, so the picture has to sit in that part. amdgpu hands VRAM out from the top in
    # 16 MB steps; fill the top with dumb buffers (held until the crash) until a new one lands low enough.
    memfd = os.open("/dev/mem", os.O_RDONLY)
    hold, hint, prev = [], None, None
    print(f"carve-out: {CARVE_SIZE >> 20} MiB at {CARVE:#x}")
    # Big buffers are placed at the bottom of the VRAM (amdgpu hands out only the small ones from the top), so the top is filled with picture-sized
    # buffers: a few hundred of them with a 4 GiB carve-out, each one predicted by the previous offset. ps5win: the step is
    # the last measured one (the buffer size depends on the screen's resolution), and the count is not tied to 16 MiB
    # buffers, so the picture still ends up low (stage 1 looks for it in the first 512 MiB of the carve-out).
    for n in range(CARVE_SIZE // 0x100000):
        d = CreateDumb(); d.width, d.height, d.bpp = W, H, 32
        ioctl(fd, CREATE_DUMB, d)
        mp = MapDumb(); mp.handle = d.handle
        ioctl(fd, MAP_DUMB, mp)
        mm = mmap.mmap(fd, d.size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=mp.offset)
        sig = struct.pack("<8I", *[0xC0DE0000 + (n << 8) + k for k in range(8)])
        mm[0:32] = sig
        off = find_vram(memfd, sig, hint, prev)
        print(f"buffer {n}: VRAM offset {off if off is None else hex(off)}")
        if off is None or off <= LOW_LIMIT:
            break
        hint = off - ((prev - off) if prev is not None and prev > off else 0x1000000)
        prev = off
        hold.append((d, mm))
    print(f"dumb buffer: handle {d.handle} pitch {d.pitch} size {d.size} ({len(hold)} filler buffers held)")
    if off is None or off >= 0x20000000 - 0x1000:
        # stage 1 looks for the picture only in the first 512 MiB of the carve-out: stop here (run_stage aborts, Linux keeps running)
        print(f"picture not placed low enough for stage 1 (VRAM offset {off if off is None else hex(off)})"); sys.exit(1)
    f = FbCmd(); f.width, f.height, f.pitch, f.bpp, f.depth, f.handle = W, H, d.pitch, 32, 24, d.handle
    ioctl(fd, ADDFB, f)
    mp = MapDumb(); mp.handle = d.handle
    ioctl(fd, MAP_DUMB, mp)
    mm = mmap.mmap(fd, d.size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=mp.offset)

    # Picture: the welcome screen prepared on the Mac (tools/make_welcome.py -> /root/welcome.raw, XRGB, pitch 10240) copied in one go;
    # without it the old test pattern (8 colour bars, a ramp, an (x,y) code) painted pixel by pixel (slow). Either way row 0 starts with the
    # signature pixels 0xDEADBE00+i, which stage 1 and patch_tripwire.py use to find the buffer in VRAM.
    welcome = "/run/ps5win/welcome.raw"
    t0 = time.time()
    if os.path.exists(welcome) and os.path.getsize(welcome) == d.pitch * H:
        with open(welcome, "rb") as fh:
            mm[0:d.pitch * H] = fh.read()
        mm[0:48] = struct.pack("<12I", *[0xDEADBE00 + i for i in range(8)], 0x5A355350, W, H, d.pitch)   # + "PS5Z" geometry for stage 1
        print(f"welcome screen copied in {time.time() - t0:.2f} s")
    else:
        # Plain black: Windows draws its boot logo on top of this picture, and anything else shows around the logo.
        mm[0:d.pitch * H] = bytes(d.pitch * H)
        mm[0:48] = struct.pack("<12I", *[0xDEADBE00 + i for i in range(8)], 0x5A355350, W, H, d.pitch)   # + "PS5Z" geometry for stage 1
        print(f"black screen in {time.time() - t0:.2f} s")

    sc = SetCrtc()
    conn_arr = u32s(1); conn_arr[0] = c.connector_id
    sc.set_connectors_ptr, sc.count_connectors = ctypes.addressof(conn_arr), 1
    sc.crtc_id, sc.fb_id, sc.mode_valid = crtc_id, f.fb_id, 1
    sc.mode = mode
    ioctl(fd, SETCRTC, sc)
    # 2026-11-19: one launch handed Windows a dead display link ("no signal" from the hand-over on; DP0_DP_LINK_CNTL 0,
    # DIG0 not enabled). Linux trains the DP link to the HDMI converter during this mode set and it sometimes fails
    # (see "Link Training Error" in dmesg). Check the link here, where a retry costs nothing, and retrain if needed.
    up, state = link_state()
    print("LINK after SETCRTC:", state)
    tries = 0
    while not up and tries < 4:
        tries += 1
        off = SetCrtc()
        off.crtc_id = crtc_id
        ioctl(fd, SETCRTC, off)                 # CRTC off: the next mode set is a full one, with link training
        time.sleep(0.5)
        ioctl(fd, SETCRTC, sc)
        time.sleep(0.5)
        up, state = link_state()
        print(f"LINK retrain {tries}:", state)
    if not up and fallback_mode is not None:
        # ps5win: the faster mode's link did not train: same picture at the slower rate, with its own retries.
        print(f"LINK: falling back to @{fallback_mode.vrefresh}")
        sc.mode = fallback_mode
        tries = 0
        while not up and tries < 4:
            tries += 1
            off = SetCrtc()
            off.crtc_id = crtc_id
            ioctl(fd, SETCRTC, off)
            time.sleep(0.5)
            ioctl(fd, SETCRTC, sc)
            time.sleep(0.5)
            up, state = link_state()
            print(f"LINK fallback {tries}:", state)
    print("LINK: up" if up else "LINK: DOWN")
    print("SETCRTC done: linear framebuffer on screen. Holding the fd (Ctrl-C / kill to release).")
    sys.stdout.flush()
    while True:
        time.sleep(3600)

if __name__ == "__main__":
    main()
