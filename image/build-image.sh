#!/bin/bash
# SPDX-License-Identifier: MIT
# Builds the Toshiba image: Docker (x86_64 Arch) runs assemble-inside.sh on the base system in volume ps5-installer-build.
# Inputs: collect/kernel (from the console's Linux: bzImage, modules/<ver>, firmware/amdgpu, initrd-modules.txt,
# bootfiles/ optional), ../build/boot33 (launch package, PS5WIN_PACKAGE), ../build/installer-tree/installer (installer session; PS5WIN_TREE picks another tree folder).
# Output: ../build/image-out/ps5-windows-installer.img (+ .sha256)
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p ../build/image-out
docker run --rm --platform linux/amd64 --privileged -e IMG_TZ=${IMG_TZ:-America/Toronto} -e PACMAN_MIRROR=${PACMAN_MIRROR:-https://geo.mirror.pkgbuild.com} \
  -v ps5-installer-build:/build -v "$PWD":/image:ro -v "$PWD/collect/kernel":/in/kernel:ro \
  -v "$PWD/../build/${PS5WIN_PACKAGE:-boot34}":/in/package:ro -v "$PWD/../build/${PS5WIN_TREE:-installer-tree}/installer":/in/installer:ro \
  -v "$PWD/../build/image-out":/out archlinux:latest bash /image/assemble-inside.sh
(cd ../build/image-out && shasum -a 256 ps5-windows-installer.img > ps5-windows-installer.img.sha256 && cat ps5-windows-installer.img.sha256)
