#!/bin/bash
# Build ps5gpu.sys (console and VM rehearsal variants) and ps5gpu.exe with mingw-w64, then test-sign the drivers.
set -eu
cd "$(dirname "$0")"
CC=x86_64-w64-mingw32-gcc
KINC=../build/gpusrc/linux/drivers/gpu/drm/amd/include
out=../build/ps5gpu
DDK="$(dirname "$($CC -print-file-name=libntoskrnl.a)")/../include/ddk"
mkdir -p "$out"
drv_flags="-I$DDK -O2 -Wall -Wextra -Werror -std=gnu11 -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -mno-stack-arg-probe
  -nostdlib -shared -Wl,--subsystem,native -Wl,--entry,DriverEntry -Wl,--image-base,0x140000000 -Wl,--dynamicbase -Wl,--nxcompat
  -Wl,--file-alignment,0x1000 -Wl,--section-alignment,0x1000 -Wl,--exclude-all-symbols -Wl,--strip-all"
$CC $drv_flags kmd/ps5gpu.c -o "$out/ps5gpu-unsigned.sys" -lntoskrnl -lhal
$CC $drv_flags -DPS5GPU_VM kmd/ps5gpu.c -o "$out/ps5gpu-vm-unsigned.sys" -lntoskrnl -lhal
$CC -O2 -Wall -Wextra -Werror -I"$KINC" tool/ps5gpu.c -o "$out/ps5gpu.exe" -static -s
# Stage 2 (user-mode bring-up). The microcode direct load is our clean-room MIT CyanDirectLoad (cleanroom/directload-mit;
# it replaced MetalCyan's non-commercial one on 2026-11-22, proven on the console in run 5).
MC=../cleanroom/directload-mit
x86_64-w64-mingw32-g++ -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -I"$KINC" -I"$KINC/../amdgpu" -I"$MC" \
  tool/stage2.cpp tool/libtest.cpp lib/ps5drm.cpp tool/backend_win.cpp "$MC/CyanDirectLoad.cpp" -o "$out/ps5gpu2.exe" -static -s

# Test certificate (made once, kept out of the source tree). Windows accepts it only with test-signing on.
if [ ! -f "$out/testcert.pem" ]; then
  openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes -keyout "$out/testkey.pem" -out "$out/testcert.pem" \
    -subj "/CN=PS5GPU Test Signing" -addext "extendedKeyUsage=codeSigning" -addext "basicConstraints=critical,CA:FALSE" 2>/dev/null
fi
for n in ps5gpu ps5gpu-vm; do
  rm -f "$out/$n.sys"
  osslsigncode sign -certs "$out/testcert.pem" -key "$out/testkey.pem" -h sha256 -n "PS5 GPU shim" -in "$out/$n-unsigned.sys" -out "$out/$n.sys" >/dev/null
done
ls -l "$out"/*.sys "$out"/ps5gpu.exe "$out"/ps5gpu2.exe
