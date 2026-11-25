#!/bin/bash
#
# Run on the PS5 (as root, main kernel with crashkernel=400M): put the linear test framebuffer on screen, load a stage-1
# chain image as the crash kernel (optionally with a bundle as its initrd) and crash into it.
#
# Usage: [GFXCLK_MHZ=2000] run_stage.sh /run/ps5win/chainV5.bzImage [/run/ps5win/ps5-upl.bundle]
set -euo pipefail
cd /run/ps5win
test "$(findmnt -n -o FSTYPE -T .)" = tmpfs
test "$(uname -r)" = 7.2.3-p5f6b
test -L /sys/bus/platform/drivers/ps5-fan/ps5-fan
test "$(cat /sys/kernel/kexec_crash_loaded)" = 0
grep -Eq '^370000000-46fffffff : Reserved$' /proc/iomem
sha256sum -c launch.sha256
# The disk is the one the PS5 Windows installer set up, described by /etc/ps5win.conf (DISK_SERIAL, BOOT_PARTUUID,
# WIN_PARTUUID; written by the installer, never sourced as shell). Rules: that disk, on USB, a vfat boot partition,
# both Windows partitions on it and neither mounted here. No file: nothing to start.
winpart=
conf=${PS5WIN_CONF:-/etc/ps5win.conf}
if [ -f "$conf" ]; then
  cget() { sed -n "s/^$1=\([A-Za-z0-9._-]*\)\$/\1/p" "$conf" | head -n 1; }
  serial=$(cget DISK_SERIAL); bootu=$(cget BOOT_PARTUUID); winu=$(cget WIN_PARTUUID)
  [ -n "$serial" ] && [ ${#bootu} = 36 ] && [ ${#winu} = 36 ] || { echo "ABORT: $conf incomplete"; exit 1; }
  bp=$(readlink -f "/dev/disk/by-partuuid/$bootu"); winpart=$(readlink -f "/dev/disk/by-partuuid/$winu")
  test -b "$bp" && test -b "$winpart"
  test "$(lsblk -nro PKNAME "$bp")" = "$(lsblk -nro PKNAME "$winpart")"
  test "$(lsblk -dnro SERIAL "/dev/$(lsblk -nro PKNAME "$bp")")" = "$serial"
  test "$(lsblk -dnro TRAN "/dev/$(lsblk -nro PKNAME "$bp")")" = usb
  test "$(lsblk -nro FSTYPE "$bp")" = vfat
  if findmnt -rn -S "$bp" >/dev/null || findmnt -rn -S "$winpart" >/dev/null; then echo "ABORT: a Windows partition is mounted"; exit 1; fi
  have=1
else
  echo "ABORT: no $conf (Windows has not been installed on this disk by the PS5 Windows installer)"; exit 1
fi
test "$have" = 1

# Installer proposal (2026-11-21): the firmware's kernel stub must match Windows' ntoskrnl (a silent "Not Found" ran
# Windows without the per-CPU fix on 2026-11-20/21). kstub-table.txt (kstub_table.py list, in the package) names every
# kernel this firmware has a stub for. Read ntoskrnl's PE header through a READ-ONLY ntfs3 mount (never rw: a refused
# rw ntfs3 mount marks the volume dirty), unmount, compare. Mismatch: stop here, Linux keeps running. A volume that
# cannot be read (e.g. hibernated) only warns: the firmware stops on a missing stub as the second wall.
# v1.0.1: a kernel NOT in the table is allowed when the firmware's pattern stub fits it (the same four checks as
# firmware KpatInstall and installer/lib/kpattern.py); otherwise ABORT naming the failed check.
if [ -n "$winpart" ] && [ -f kstub-table.txt ]; then
  ro=/run/ps5win/winro; mkdir -p "$ro"
  if mount -t ntfs3 -o ro,noexec,nosuid,nodev "$winpart" "$ro" 2>/dev/null; then
    # 2026-11-23: a kernel not in the table gets the firmware's pattern stub when the same four checks pass here
    # (build/kpattern/kfind.py: entry bytes, POOLCODE cave, HalpLocalApic accessors, HAL probe; each exactly once).
    kinfo=$(python3 - "$ro/Windows/System32/ntoskrnl.exe" <<'PY' || true
import re, struct, sys
b = open(sys.argv[1], "rb").read()
e = struct.unpack_from("<I", b, 0x3C)[0]
ts, ep, soi = struct.unpack_from("<I", b, e + 8)[0], struct.unpack_from("<I", b, e + 40)[0], struct.unpack_from("<I", b, e + 80)[0]
secs = []
for i in range(struct.unpack_from("<H", b, e + 6)[0]):
    o = e + 24 + struct.unpack_from("<H", b, e + 20)[0] + 40 * i
    vs, va, rs, ro = struct.unpack_from("<IIII", b, o + 8)
    secs.append((b[o:o + 8].rstrip(b"\0"), va, vs, ro, rs, struct.unpack_from("<I", b, o + 36)[0]))
off = lambda rva: next(ro + rva - va for _, va, vs, ro, rs, _ in secs if va <= rva < va + max(vs, rs))
rva = lambda o: next(va + o - ro for _, va, vs, ro, rs, _ in secs if ro <= o < ro + rs)
why = []
if b[off(ep):off(ep) + 12] != bytes.fromhex("4883ec384c897c24304c8bfc"): why.append("entry")
cave = [b[ro + vs:ro + min(rs, (vs + 0xFFF) & ~0xFFF)] for n, va, vs, ro, rs, ch in secs
        if n == b"POOLCODE" and ch & 0x20000020 == 0x20000020 and not ch & 0x02000000 and ((vs + 0xFFF) & ~0xFFF) - vs >= 0x200]
if len(cave) != 1 or not set(cave[0]) <= {0, 0xCC}: why.append("cave")
text = [(ro, rs) for n, va, vs, ro, rs, ch in secs if ch & 0x20000000]
v = {}
for name, rx in (("r", rb"\x48\x8b\x05(....)\x48\x63\xd1\x8b\x04\x02\xc3"), ("w", rb"\x48\x8b\x05(....)\x4c\x63\xc1\x41\x89\x14\x00\xc3")):
    v[name] = [rva(m.start()) + 7 + struct.unpack("<i", m.group(1))[0] for ro, rs in text for m in re.compile(rx, re.S).finditer(b, ro, ro + rs)]
if len(v["r"]) != 1 or v["r"] != v["w"]: why.append("apic")
def fnv(x):
    h = 0xcbf29ce484222325
    for c in x: h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h
probe = [i for ro, rs in text for i in range(ro, ro + rs - 44) if b[i] == 0xC7 and b[i + 1] == 0x03 and fnv(b[i:i + 44]) == 0xb52d15455196d1b1]
if len(probe) != 1: why.append("halprobe")
print("0x%08x 0x%x %s" % (ts, soi, ",".join(why) or "ok"))
PY
)
    kid=${kinfo% *}; kpat=${kinfo##* }
    umount "$ro"
    if findmnt -rn -S "$winpart" >/dev/null; then echo "ABORT: could not unmount the read-only Windows mount"; exit 1; fi
    if [ -n "$kid" ] && grep -q "^$kid " kstub-table.txt; then
      echo "Kernel stub: Windows kernel $kid matches $(grep "^$kid " kstub-table.txt | cut -d' ' -f3)"
    elif [ -n "$kid" ] && [ "$kpat" = ok ]; then
      echo "Kernel stub: Windows kernel $kid is not in the table; entry, cave, APIC pointer and HAL probe each found once: the firmware uses its pattern stub"
    else
      echo "ABORT: Windows kernel ${kid:-unreadable} has no table stub and fails the pattern checks (${kpat:-unreadable}); nothing was loaded, Linux keeps running"
      exit 1
    fi
  else
    echo "WARNING: could not mount the Windows partition read-only to check its kernel; the firmware checks it again"
  fi
fi

IMG="${1:?chain bzImage}"
BUNDLE="${2:-}"

systemctl stop hackintosh 2>/dev/null || true
# Local (CachyOS ps5-linux-loader image): tty1 autologins "steam" and /etc/profile.d/ps5-tty-session.sh runs KDE Plasma (KWin holds
# DRM master, so kms_linear.py could not take the screen). If Plasma merely exits, that script writes "gamescope" into
# ~/.config/ps5-next-session and execs gamescope (black screen on this console, and it grabs DRM again). So: stop the getty first
# (no respawn), then SIGKILL the whole seat0 session (the shell dies before it can run anything), and put the session file back.
systemctl stop getty@tty1 2>/dev/null || true
for s in $(loginctl list-sessions --no-legend 2>/dev/null | awk '$4=="seat0"{print $1}'); do loginctl kill-session "$s" --signal=SIGKILL || true; done
pkill -KILL -x 'kwin_wayland|kwin_wayland_wrapper|plasmashell|startplasma-wayland|gamescope|gamescope-wl|gamescope-session-ps5' || true
# a SIGKILLed compositor takes a moment to let go of the GPU (attempt 2, 2026-11-14: SET_MASTER EBUSY 2 s after the kill): wait until no
# process has the DRM card open, then take it, retrying a few times
for i in $(seq 1 30); do fuser -s /dev/dri/card0 2>/dev/null || break; sleep 0.5; done
fuser -v /dev/dri/card0 2>&1 | tail -n +2 | sed 's/^/still holding the GPU: /' || true
kms_ok=0
for try in 1 2 3 4 5; do
    nohup python3 /run/ps5win/kms_linear.py >/run/ps5win/kms.log 2>&1 &
    kpid=$!
    # wait for the picture to be on screen (a few seconds with the prepared welcome screen), not a fixed time
    for i in $(seq 1 400); do grep -q "SETCRTC done" /run/ps5win/kms.log 2>/dev/null && { kms_ok=1; break; }; kill -0 $kpid 2>/dev/null || break; sleep 0.5; done
    [ $kms_ok = 1 ] && break
    echo "kms_linear try $try: $(tail -1 /run/ps5win/kms.log)"; sleep 2
done
sleep 1
tail -1 /run/ps5win/kms.log
grep '^LINK' /run/ps5win/kms.log || true
# 2026-11-19: do not hand over a dead display link (LINK_FORCE=1 skips this, if the check itself turns out to be wrong)
if [ $kms_ok = 1 ] && grep -q '^LINK: DOWN' /run/ps5win/kms.log && [ "${LINK_FORCE:-0}" != 1 ]; then
    echo "ABORT: the display link is down after the mode set (see the LINK lines above); nothing was loaded, Linux keeps running"
    exit 1
fi
if [ $kms_ok != 1 ]; then
    # no picture for UEFI/macOS: crashing now only wastes a power cycle. Give the console its desktop back and stop.
    systemctl start getty@tty1 2>/dev/null || true
    echo "ABORT: the linear framebuffer is not on screen (see /run/ps5win/kms.log); nothing was loaded, Linux keeps running"
    exit 1
fi
# Boot 27 (2026-11-20): the GPU clock. Linux's ps5-gpufreq driver scales it with load (500-2230 MHz); the desktop is idle here, so
# Windows would inherit whatever the governor chose last (nothing under Windows changes it). Always print the state; with
# GFXCLK_MHZ=<500..2230> pin the governor to that clock first (its own sysfs limits, i.e. the driver's own RequestGfxclk), and stop
# before anything is loaded if the clock does not get there. Without GFXCLK_MHZ nothing is changed (as boot 26).
df=/sys/class/devfreq/ps5-gpufreq
if [ -d "$df" ]; then
    echo "GPU clock: now $(( $(cat $df/cur_freq) / 1000000 )) MHz, target $(( $(cat $df/target_freq) / 1000000 )), governor $(cat $df/governor), limits $(( $(cat $df/min_freq) / 1000000 ))-$(( $(cat $df/max_freq) / 1000000 )) MHz"
    echo "GPU clock steps: $(cat $df/available_frequencies)"
    if [ -n "${GFXCLK_MHZ:-}" ]; then
        hz=$(( GFXCLK_MHZ * 1000000 ))
        if [ "$GFXCLK_MHZ" -lt 500 ] || [ "$GFXCLK_MHZ" -gt 2230 ] || ! grep -qw "$hz" $df/available_frequencies; then
            echo "ABORT: $GFXCLK_MHZ MHz is not one of the driver's clock steps (500-2230); nothing was loaded, Linux keeps running"
            exit 1
        fi
        echo "$hz" > $df/min_freq
        echo "$hz" > $df/max_freq
        ok=0
        for i in $(seq 1 20); do [ "$(cat $df/cur_freq)" = "$hz" ] && { ok=1; break; }; sleep 0.25; done
        echo "GPU clock: asked for $GFXCLK_MHZ MHz, now $(( $(cat $df/cur_freq) / 1000000 )) MHz"
        if [ $ok != 1 ]; then
            echo "ABORT: the GPU clock did not reach $GFXCLK_MHZ MHz; nothing was loaded, Linux keeps running"
            exit 1
        fi
    fi
else
    echo "GPU clock: no $df here; left alone"
fi
# a tripwire kernel patch in the OpenCore config needs this boot's framebuffer offset

python3 - "$IMG" "$BUNDLE" <<'PY'
import ctypes, os, subprocess, sys
libc = ctypes.CDLL(None, use_errno=True)
kfd = os.open(sys.argv[1], os.O_RDONLY)
# The same command line serves a later "return to Linux" (bringup/retlinux), which boots without an initramfs: name the root by its
# PARTUUID (the kernel resolves that itself; sdX letters can swap between boots and LABEL= needs the initramfs). 2026-11-15.
try:
    pu = subprocess.run(["findmnt", "-no", "PARTUUID", "/"], capture_output=True, text=True).stdout.strip()
except OSError:
    pu = ""
root = ("PARTUUID=" + pu) if len(pu) == 36 else "/dev/sda1"
print("crash/return kernel root:", root)
cmd = ("root=%s rw rootwait console=tty0 mitigations=off idle=halt nr_cpus=1 module_blacklist=amdgpu systemd.mask=hackintosh.service systemd.unit=multi-user.target" % root).encode() + b"\0"
if sys.argv[2]:
    ifd, flags = os.open(sys.argv[2], os.O_RDONLY), 2          # KEXEC_FILE_ON_CRASH
else:
    ifd, flags = -1, 6                                          # ON_CRASH | NO_INITRAMFS
if open("/sys/kernel/kexec_crash_loaded").read().strip() != "0":
    raise SystemExit("A crash image is already loaded")
r = libc.syscall(320, kfd, ifd, len(cmd), cmd, flags)
print("kexec_file_load:", r, os.strerror(ctypes.get_errno()), "crash_loaded:", open("/sys/kernel/kexec_crash_loaded").read().strip())
if r or open("/sys/kernel/kexec_crash_loaded").read().strip() != "1":
    raise SystemExit("The verified Windows stage did not load")
PY
sync
echo 1 > /proc/sys/kernel/sysrq
# the CPU that takes the crash becomes the one running UEFI and macOS: it has to be the boot processor (CPU 0), because XNU
# panics with "Unexpected local APIC state" in lapic_init() on any other core (IA32_APIC_BASE.BSP clear)
# Sony's GbE (104d:9104) keeps its DMA rings armed through a crash and the IOMMU is switched off afterwards: its ring addresses would
# become physical addresses inside macOS' memory. Linux's mts driver stops both DMA engines on "ip link set down" (this ends the ssh session).
# Local: the internal Wi-Fi at 40:00.7 is not a Sony function: launch consoles ship either a Marvell IW620 (mwifiex_pcie) or, like
# this one, a MediaTek MT7921 (14c3:7961, mt7921e). Driven by Linux, it has the same problem as the GbE (RX rings armed in RAM that
# macOS reuses once the IOMMU is off), and "down" alone may leave its DMA running: unbind every non-Sony PCI network function from
# its driver (the remove path stops DMA and disables the function). This ends a Wi-Fi ssh session, so it runs in the detached crash job.
nohup taskset -c 0 sh -c 'sleep 1; for n in /sys/class/net/*; do [ "$(cat $n/device/vendor 2>/dev/null)" = 0x104d ] && ip link set "${n##*/}" down; done
for n in /sys/class/net/*; do d=$(readlink -f $n/device 2>/dev/null) || continue; [ -e "$d/vendor" ] && [ -e "$d/driver/unbind" ] || continue
  [ "$(cat $d/vendor)" = 0x104d ] && continue; [ "$(cat $d/class)" = 0x028000 ] || [ "$(cat $d/class)" = 0x020000 ] || continue
  echo "${d##*/}" > "$d/driver/unbind"; done
# Windows (2026-11-20): the unbind above also clears the bus-master bit of the Wi-Fi chip, and the Windows driver cannot set it on
# the real function (that needs the config window in 40:00.0, which is never touched). The chip is quiet now (DMA engines off),
# so give it memory decoding + bus master back, as macOS does from its side. Only the MT7921 function, only its command register.
for d in /sys/bus/pci/devices/*; do [ "$(cat $d/vendor)" = 0x14c3 ] && [ "$(cat $d/device)" = 0x7961 ] && printf "\006\004" | dd of=$d/config bs=1 seek=4 count=2 conv=notrunc 2>/dev/null; done
# Local: the ps5-fan driver (ps5-linux-patches drivers/ps5/fan.c) keeps the fan zone in MANUAL mode and writes a duty every 500 ms from
# its own curve (0 = stopped below ~45 C); its 90 C full-speed emergency is in that loop too, not in the firmware. A crash runs none of
# its shutdown hooks, so macOS would inherit MANUAL at the last duty with nothing watching. Unbinding the driver runs its own cleanup
# (ps5_fan_hwmon_cleanup: restore the console servo table, mode AUTO), so the southbridge firmware drives the fan under macOS.
# If that is not possible, pwm1_enable=0 puts it at full speed (loud, safe).
set -e
test -L /sys/bus/platform/drivers/ps5-fan/ps5-fan
echo ps5-fan > /sys/bus/platform/drivers/ps5-fan/unbind
test ! -e /sys/bus/platform/drivers/ps5-fan/ps5-fan
sync
echo c > /proc/sysrq-trigger' >/dev/null 2>&1 &
echo "crashing into the stage in 1 s"
