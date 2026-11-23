#!/bin/bash
# SPDX-License-Identifier: MIT
# Starts Windows from this disk, on the console itself (no Mac): what launch-windows.sh did over SSH.
# The launch package (/opt/ps5win/package/deploy.tgz: firmware bundle, stage kernel, kms_linear.py, run_stage.sh,
# kstub-table.txt, launch.sha256) is unpacked into RAM, checked, and started once Linux has been up 150 s (an earlier
# start fails safely with "SET_MASTER ... busy"). run_stage.sh refuses anything it does not recognise (disk identity from
# /etc/ps5win.conf, Windows kernel without a stub, GPU clock not reached); then Linux keeps running.
# Usage: ps5win-start.sh   (GFXCLK_MHZ=0 leaves the GPU clock alone; default 2000)
set -u
pkg=/opt/ps5win/package/deploy.tgz
run=/run/ps5win
logdir=/var/log/ps5win
mkdir -p "$logdir"
log="$logdir/launch-$(date +%Y%m%d-%H%M%S).txt"
say() { echo "$*" | tee -a "$log"; }

[ -f /etc/ps5win.conf ] || { say "No Windows on this disk yet (/etc/ps5win.conf missing)."; exit 1; }
rm -rf "$run" && mkdir -p "$run" && tar xzmf "$pkg" -C "$run" || { say "Cannot unpack $pkg"; exit 1; }
(cd "$run" && sha256sum -c --quiet launch.sha256) >>"$log" 2>&1 || { say "Launch package check failed; nothing started."; exit 1; }
# Wait for the GPU driver (display device and its clock controls) and at least 30 s of uptime; never longer than 150 s.
# The old fixed 150 s was for a desktop Linux whose compositor holds DRM master for a minute or two after boot; this
# image has no desktop, and run_stage.sh retries the display take-over itself if something briefly holds it.
gpu_ready() { [ -e /dev/dri/card0 ] && [ -r /sys/class/devfreq/ps5-gpufreq/available_frequencies ]; }
up=$(cut -d. -f1 /proc/uptime)
while [ "$up" -lt 150 ] && { [ "$up" -lt 30 ] || ! gpu_ready; }; do
  printf '\rWaiting for Linux to settle: %3d s ' "$up"
  sleep 1; up=$(cut -d. -f1 /proc/uptime)
done
echo
# The Windows Ethernet driver (p5eth) uses the port's PHY as Linux leaves it: it does no PHY bring-up itself. A desktop
# Linux opens the port through its network manager; this one has no network setup, so open the Sony GbE port here
# (link only: no address, no DHCP) and give it a few seconds to negotiate. run_stage.sh takes it down again before the
# hand-over (DMA stopped); the PHY stays initialised for Windows.
for n in /sys/class/net/*; do
  # Sony's GbE function (PCI 104d:9104), driven by Linux's `mts`. Matched by vendor + driver name: its PCI class code is
  # not the generic Ethernet one (a class check skipped it on 2026-11-22).
  [ "$(cat "$n/device/vendor" 2>/dev/null)" = 0x104d ] || continue
  drv=$(basename "$(readlink -f "$n/device/driver" 2>/dev/null)" 2>/dev/null)
  say "Ethernet ${n##*/}: driver ${drv:-none}, class $(cat "$n/device/class" 2>/dev/null)"
  [ "$drv" = mts ] || continue
  ip link set "${n##*/}" up 2>>"$log" && say "Ethernet ${n##*/}: port opened for Windows"
  for t in 1 2 3 4 5 6 7 8 9 10; do [ "$(cat "$n/carrier" 2>/dev/null)" = 1 ] && break; sleep 1; done
  say "Ethernet ${n##*/}: carrier $(cat "$n/carrier" 2>/dev/null || echo ?)"
done
# Display: the screen's own (preferred) resolution, up to 3840x2160, at the highest refresh rate it offers up to
# 120 Hz (kms_linear.py). Optional lines in ps5win-display.txt on the PS5ISO partition: "max_hz=60" caps the refresh
# rate (for screens that show no picture at 120 Hz), "mode=2560x1440" picks another resolution the screen offers.
# Release default 2026-11-22: 60 Hz (the 120 Hz display driver isn't released yet; the shipped driver reports 60 Hz).
export PS5WIN_MAX_HZ=60
if [ -f /mnt/ps5iso/ps5win-display.txt ]; then
  hz=$(sed -n 's/^max_hz=\([0-9]\{2,3\}\)\r\?$/\1/p' /mnt/ps5iso/ps5win-display.txt | head -n 1)
  [ -n "$hz" ] && export PS5WIN_MAX_HZ=$hz && say "Display: refresh capped at $hz Hz (ps5win-display.txt)"
  mode=$(sed -n 's/^mode=\([0-9]\{3,4\}x[0-9]\{3,4\}\)\r\?$/\1/p' /mnt/ps5iso/ps5win-display.txt | head -n 1)
  [ -n "$mode" ] && export PS5WIN_MODE=$mode && say "Display: $mode asked for (ps5win-display.txt)"
fi
say "Starting Windows (GPU clock ${GFXCLK_MHZ:-2000} MHz)..."
cd "$run" && GFXCLK_MHZ=${GFXCLK_MHZ:-2000} bash run_stage.sh "$run/windows.bzImage" "$run/windows.bundle" 2>&1 | tee -a "$log"
rc=${PIPESTATUS[0]}
# run_stage.sh returns right after it starts the background job that crashes into the stage ~1 s later. Stay alive
# meanwhile: if this script ended, systemd would stop the service and kill that job (seen on the Toshiba, 2026-11-21).
if [ "$rc" = 0 ] && [ "$(cat /sys/kernel/kexec_crash_loaded 2>/dev/null)" = 1 ]; then
  say "Handing over to Windows..."
  sync
  sleep 60
fi
say "Windows did not start; Linux keeps running. Log: $log"
exit 1
