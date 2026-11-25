#!/bin/bash
# Builds ps5d3d12.dll with mingw-w64 into build/wddm2/d3d12umd (ARCH=x86: 32-bit, into build/wddm2/d3d12umd32), next
# to ps5vkd3d.dll (vkd3d-proton 3.1's d3d12core.dll with ../vkd3d-patches, see the README there, renamed). Headers: Microsoft's d3d12umddi.h from the local kit (build/wdk, not redistributed) and the
# winddk shims already used for the Mesa D3D10 driver.
set -euo pipefail
cd "$(dirname "$0")"
root=../../..
if [ "${ARCH:-x64}" = x86 ]; then cc=i686-w64-mingw32; out=${OUT:-$root/build/wddm2/d3d12umd32}; vk=$root/build/wddm2/vkd3d-fix/x86/ps5vkd3d.dll
else cc=x86_64-w64-mingw32; out=${OUT:-$root/build/wddm2/d3d12umd}; vk=$root/build/wddm2/vkd3d-fix/ps5vkd3d.dll; fi
mkdir -p "$out"
python3 ../tools/genstubs.py "$root/build/wdk/x-wdk.x64/c/Include/10.0.28000.0/um/d3d12umddi.h" "$root/build/wdk/x-sdk.cpp/c/Include/10.0.28000.0/um/dxgiddi.h" > stubs.inc
$cc-g++ -std=c++17 -O2 -Wall -Wno-unknown-pragmas -Wno-unused-function -shared \
  -I "$root/build/gpusrc/mesa/include/winddk" -I "$root/build/wdk/x-wdk.x64/c/Include/10.0.28000.0/um" \
  ps5d3d12.cpp -o "$out/ps5d3d12.dll" -static -static-libgcc -static-libstdc++ -Wl,--kill-at -ldxguid -lgdi32
cp "$vk" "$out/ps5vkd3d.dll"
$cc-objdump -p "$out/ps5d3d12.dll" | grep -E 'DLL Name|OpenAdapter12'
ls -l "$out"
