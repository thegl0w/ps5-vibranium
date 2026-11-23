#!/bin/bash
# SPDX-License-Identifier: MIT
# Runs inside the x86_64 Arch container: installs the base system into /build/root (a Docker volume, case-sensitive).
set -euo pipefail
# pacman's download sandbox (seccomp/landlock, user alpm) does not work under x86 emulation: off for the build only.
sed -i -e 's/^#DisableSandbox/DisableSandbox/' -e 's/^DownloadUser/#DownloadUser/' /etc/pacman.conf
grep -q '^DisableSandbox' /etc/pacman.conf || sed -i '/^\[options\]/a DisableSandbox' /etc/pacman.conf
pacman -Sy --noconfirm --needed arch-install-scripts >/dev/null
mkdir -p /build/root /build/pkgcache
pkgs=$(grep -v '^#' /image/packages.txt | xargs)
pacstrap -K -c /build/root $pkgs --cachedir /build/pkgcache 2>&1 | tail -5
du -sh /build/root
pacman --root /build/root -Q | wc -l
pacman --root /build/root -Q > /build/packages-installed.txt
