#!/bin/bash
# Configure the PS5 build of Mesa's RADV (run from windows-handoff/). Needs: build/gpusrc/mesa (mesa-26.3.1 with
# mesa-ps5.patch applied and src/amd/ps5gpu -> ../../../../../ps5gpu), build/gpusrc/venv (mako, PyYAML, packaging),
# mingw-w64, glslang, meson, ninja. Then: ninja -C build/gpusrc/mesa-build-ps5
#
#   -mbmi -mlzcnt        addrlib uses _tzcnt_u32 on Windows; the PS5's Zen 2 has both
#   -mno-ms-bitfields    Mesa packs shader keys in bit fields of mixed types and copies them as one integer; with the
#                        MSVC-style layout mingw uses by default the fields do not fit and options are lost
#                        (2026-11-19: the buffer-fill shader was built without addr_user_data and read address 0)
#   radv-build-id        Windows has no ELF build id for the cache key
set -eu
cd "$(dirname "$0")/../../build/gpusrc"
export PATH="$PWD/venv/bin:$PATH"
meson setup mesa-build-ps5 mesa --cross-file mingw-cross.txt -Dbuildtype=release -Dvulkan-drivers=amd -Dgallium-drivers= \
  -Dplatforms=windows -Dllvm=disabled -Dzlib=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dopengl=false -Dgles1=disabled \
  -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dbuild-tests=false -Dvideo-codecs= -Dvulkan-layers= -Dshader-cache=disabled \
  -Dspirv-tools=disabled -Dwerror=false -Dc_args='-mbmi -mlzcnt -mno-ms-bitfields' -Dcpp_args='-mbmi -mlzcnt -mno-ms-bitfields' \
  -Dps5gpu=true -Dradv-build-id=5053354750553031
