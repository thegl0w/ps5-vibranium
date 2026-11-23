#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/edk2
if [ ! -f build/edk2/UefiPayloadPkg/Ps5Payload.dsc ]; then
  rsync -a --exclude=.git --exclude=Build ../ps5-hackintosh/build/edk2/ build/edk2/
fi
python3 firmware/prepare.py
docker run --rm -v "$PWD/build/edk2:/work/edk2" -v "$PWD/../ps5-hackintosh/uefi:/work/ps5:ro" -w /work/edk2 ps5-edk2 bash -lc '
  export PACKAGES_PATH=/work/edk2:/work/ps5
  source edksetup.sh >/dev/null && python3 UefiPayloadPkg/UniversalPayloadBuild.py -t CLANGDWARF -c UefiPayloadPkg/Ps5Payload.dsc -b RELEASE -l 1744830464 -D TIMER_SUPPORT=LAPIC -D SERIAL_DRIVER_ENABLE=FALSE -D PS2_KEYBOARD_ENABLE=FALSE -D PS2_MOUSE_ENABLE=FALSE -D SD_ENABLE=FALSE -D ATA_ENABLE=FALSE -D NVME_ENABLE=FALSE -D NETWORK_DRIVER_ENABLE=FALSE -D MAX_LOGICAL_PROCESSORS=1 -D PCIE_BASE_SUPPORT=FALSE' >build/firmware-build.log 2>&1
