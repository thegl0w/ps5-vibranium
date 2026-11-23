#!/bin/bash
# Build ps5wddm.sys on the Mac: clang-cl + lld-link against Microsoft's driver kit (unpacked NuGet packages in
# build/wdk, see NEXT.md), then test-sign it with the same local certificate as ps5gpu.sys.
set -euo pipefail
cd "$(dirname "$0")"
root=../..
out=${OUT:-$root/build/ps5gpu/wddm}   # OUT=<folder> builds somewhere else (the release takes build/ps5gpu/wddm)
kit=$root/build/wdk
W=$kit/x-wdk.x64/c/Include/10.0.28000.0
S=$kit/x-sdk.cpp/c/Include/10.0.28000.0
L=$kit/x-wdk.x64/c/Lib/10.0.28000.0/km/x64
LLVM=/opt/homebrew/opt/llvm/bin
LLD=/opt/homebrew/opt/lld/bin
mkdir -p "$out"
CF="--target=x86_64-pc-windows-msvc /c /kernel /GS- /Zl /O2 /W3 /WX /GR- -Wno-unused-parameter /D_AMD64_ /D_WIN64 /DNTDDI_VERSION=0x0A000008 /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DDXGKDDI_INTERFACE_VERSION=0x5023"
INC=(/imsvc "$W/km/crt" /imsvc "$W/km" /imsvc "$W/shared" /imsvc "$S/shared")
# The boot-time GPU start (gpuboot.cpp) and the clean-room microcode loader it calls, built for the kernel: Linux's
# register headers (MIT), the loader from cleanroom/ as it is, and kcompat/ for the three C++ headers they include.
KINC=$root/build/gpusrc/linux/drivers/gpu/drm/amd/include
MC=$root/cleanroom/directload-mit
GINC=(/I kcompat /I "$MC" /I "$KINC" /I "$KINC/../amdgpu")
# The 28000 kit's libraries carry "function override" tables (sections _guard_check_icall_, _guard_dispatch_icall_,
# memset_) that Microsoft's linker folds away. lld leaves them as sections without access flags; put them in .rdata.
LF="/nologo /driver /merge:_guard_check_icall_=.rdata /merge:_guard_dispatch_icall_=.rdata /merge:memset_=.rdata /subsystem:native,10.0 /nodefaultlib /machine:x64 /release /highentropyva:no /tsaware:no /pdbaltpath:%_PDB%"   # the last two: lld sets those header flags by default and the Windows 10 loader then refuses a driver (0xC0000018; VM test 5)

# ps5wddm.sys: the driver, with its own stack-cookie check (no bufferoverflowfastfailk.lib, so no load configuration block).
"$LLVM/clang-cl" $CF /std:c++17 -Wno-unused-const-variable "${GINC[@]}" "${INC[@]}" /Fo"$out/gpuboot.obj" gpuboot.cpp
"$LLVM/clang-cl" $CF /std:c++17 "${GINC[@]}" "${INC[@]}" /Fo"$out/CyanDirectLoad.obj" "$MC/CyanDirectLoad.cpp"
"$LLVM/clang-cl" $CF /std:c++17 /DP5_OWN_COOKIE "${INC[@]}" /Fo"$out/ps5wddm.obj" ps5wddm.cpp
"$LLD/lld-link" $LF /entry:DriverEntry /out:"$out/ps5wddm-unsigned.sys" /map:"$out/ps5wddm.map" /pdb:"$out/ps5wddm.pdb" /debug \
  "$out/ps5wddm.obj" "$out/gpuboot.obj" "$out/CyanDirectLoad.obj" "$L/displib.lib" "$L/ntoskrnl.lib" "$L/hal.lib"
# Diagnostic variants for the VM: "a" = Microsoft's cookie library and entry point (as first tried); p5min = an empty driver.
"$LLVM/clang-cl" $CF /std:c++17 "${INC[@]}" /Fo"$out/ps5wddm-a.obj" ps5wddm.cpp
"$LLD/lld-link" $LF /entry:GsDriverEntry /out:"$out/ps5wddm-a-unsigned.sys" "$out/ps5wddm-a.obj" "$out/gpuboot.obj" "$out/CyanDirectLoad.obj" "$L/displib.lib" "$L/bufferoverflowfastfailk.lib" "$L/ntoskrnl.lib" "$L/hal.lib"
"$LLVM/clang-cl" $CF "${INC[@]}" /Fo"$out/p5min.obj" p5min.c
"$LLD/lld-link" $LF /entry:DriverEntry /out:"$out/p5min-unsigned.sys" "$out/p5min.obj" "$L/ntoskrnl.lib"
ls -l "$out/ps5wddm-unsigned.sys"
key=$root/build/ps5gpu
for n in ps5wddm ps5wddm-a p5min; do
  rm -f "$out/$n.sys"
  osslsigncode sign -certs "$key/testcert.pem" -key "$key/testkey.pem" -h sha256 -n "PS5 GPU display driver" -in "$out/$n-unsigned.sys" -out "$out/$n.sys" >/dev/null
done
# Every section must be readable.
if "$LLVM/llvm-readobj" --sections "$out/ps5wddm.sys" | grep -q 'Characteristics \[ (0x0)'; then echo "ERROR: a section without access flags"; exit 1; fi
x86_64-w64-mingw32-gcc -O2 -Wall ntload.c -o "$out/ntload.exe" -static -s -lntdll
# Every build gets its own, rising driver version (0.3.MMDD.<seconds since midnight / 2>): Windows silently keeps the
# installed binary when a package with the same version is offered again (it happened twice on 2026-11-20).
sed "s|^DriverVer=.*|DriverVer=$(date +%m/%d/%Y),0.3.$(date +%-m%d).$(( ($(date +%-H) * 3600 + $(date +%-M) * 60 + $(date +%-S)) / 2 ))|" ps5wddm.inf > "$out/ps5wddm.inf"
grep DriverVer "$out/ps5wddm.inf"
ls -l "$out/ps5wddm.sys"
