# vkd3d-proton 3.1 patches for ps5vkd3d.dll

ps5d3d12.dll forwards to vkd3d-proton 3.1's d3d12core.dll, deployed as C:\ps5\wddm2\umd\ps5vkd3d.dll.

- 0001: dxil-spirv lowered the two-output DXIL UDiv (dx.op binaryWithTwoOuts, produced by Windows' dxilconv when
  the runtime converts a game's DXBC with shader model 6.x reported) by building its UDiv/UMod helper functions on
  the struct type %dx.types.twoi32. The result was invalid SPIR-V ("OpFunctionCall Argument type does not match"), and
  RADV crashed (NIR add_defs_uses) or hung. The patch builds the helpers on uint.

Build (macOS, Homebrew mingw-w64 + meson; source from build/dl-release/vkd3d/vkd3d-proton-3.1-src.tar.xz):

    cd build/vkd3d-src/vkd3d-proton-3.1 && patch -p1 < ../../../ps5gpu/d3d12/vkd3d-patches/0001-*.patch
    meson setup --cross-file build-win64.txt --buildtype release -Denable_tests=false -Denable_extras=false ../../vkd3d-build64
    ninja -C ../../vkd3d-build64
    x86_64-w64-mingw32-strip -o ps5vkd3d.dll ../../vkd3d-build64/libs/d3d12core/d3d12core.dll

Check shaders offline: build dxil-spirv with cmake (build/dxil-spirv-mac), then
`dxil-spirv <x.cso> --output x.spv && spirv-val x.spv` over the P5D3D12_DUMP=1 dumps.
