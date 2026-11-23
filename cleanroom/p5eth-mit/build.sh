#!/bin/sh
# SPDX-License-Identifier: MIT
# Build p5eth.sys (unsigned) with clang-cl + lld-link on macOS.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
HANDOFF="$(cd "$HERE/../.." && pwd)"
WDK="$HANDOFF/build/wdk"
VER=10.0.28000.0
INC="$WDK/x-wdk.x64/c/Include/$VER"
SDKINC="$WDK/x-sdk.cpp/c/Include/$VER"
LIB="$WDK/x-wdk.x64/c/Lib/$VER/km/x64"
OUT="$HANDOFF/build/cleanroom-p5eth"

CLANG=/opt/homebrew/opt/llvm/bin/clang-cl
LLD=/opt/homebrew/opt/lld/bin/lld-link

mkdir -p "$OUT"
cd "$OUT"

# Build with relative paths from a temp copy so no absolute /Users path lands in the binary.
cp "$HERE/p5eth.cpp" "$OUT/p5eth.cpp"

"$CLANG" --target=x86_64-pc-windows-msvc /c /kernel /GS- /Zl /O2 /GR- /std:c++17 \
    /D_AMD64_ /D_WIN64 /DNTDDI_VERSION=0x0A000008 /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 \
    /DNDIS_MINIPORT_DRIVER=1 /DNDIS630_MINIPORT=1 /DNDIS_WDM=1 \
    /imsvc "$INC/km/crt" /imsvc "$INC/km" /imsvc "$INC/shared" /imsvc "$SDKINC/shared" \
    -Wno-microsoft-anon-tag -Wno-pragma-pack -Wno-ignored-pragma-intrinsic -Wno-unknown-pragmas \
    /Fo"p5eth.obj" "p5eth.cpp"

"$LLD" /driver /subsystem:native,10.0 /nodefaultlib /machine:x64 /release /highentropyva:no /tsaware:no /entry:DriverEntry \
    /pdbaltpath:%_PDB% /debug:none \
    /libpath:"$LIB" ndis.lib ntoskrnl.lib hal.lib ntstrsafe.lib \
    /out:"p5eth-unsigned.sys" "p5eth.obj"

rm -f "$OUT/p5eth.cpp" "$OUT/p5eth.obj"

if strings "$OUT/p5eth-unsigned.sys" | grep -q /Users; then
    echo "error: binary contains a /Users path" >&2
    exit 1
fi
ls -l "$OUT/p5eth-unsigned.sys"
