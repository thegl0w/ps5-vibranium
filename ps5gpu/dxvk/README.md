# DXVK patches for the PS5 game kit

Applies to DXVK v3.1.1 (tag v3.1.1, b1a1c99a), zlib licence like DXVK itself.

- `0001-d3d9-16bpp-modes-on-32bpp-displays.patch`: Windows 8+ lists no 8/16 bpp display modes for full WDDM
  drivers, so DXVK's d3d9 (and d3d8 on top of it) listed no R5G6B5 modes and old games looking for 640x480x16
  (e.g. "Cannot find 640x480 video mode") gave up. With the patch d3d9 offers the 32-bit modes for a 16-bit
  format when the display has no 16-bit modes (as native D3D9 does), and the Win32 WSI sets such a mode at 32 bpp.

Build (Mac, mingw-w64 + meson): `git clone --depth 1 --branch v3.1.1 --recurse-submodules --shallow-submodules
https://github.com/doitsujin/dxvk.git`, `git apply` the patch, `meson setup --cross-file build-win32.txt --buildtype
release <out>` (win64 likewise), `ninja src/d3d9/d3d9.dll src/d3d8/d3d8.dll`, strip. Ship d3d8 + d3d9 of one build.
