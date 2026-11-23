#!/bin/bash
# SPDX-License-Identifier: MIT
# Builds the Arch base system for the installer image in Docker (x86_64 emulated on the Mac). Downloads packages from
# the Arch mirrors (approved by the user 2026-11-21). Output stays in the Docker volume ps5-installer-build (/build/root).
set -euo pipefail
cd "$(dirname "$0")"
docker run --rm --platform linux/amd64 --privileged -v ps5-installer-build:/build -v "$PWD":/image:ro archlinux:latest bash /image/base-inside.sh
