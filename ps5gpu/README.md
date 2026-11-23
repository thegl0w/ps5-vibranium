# ps5gpu: GPU driver work for Windows on the PS5 (route O in ../GPU-STUDY.md)

- `kmd/ps5gpu.c`: kernel shim `ps5gpu.sys`. Finds 20:00.0 (1002:13FC), maps the register BAR, and gives user mode
  register access through a firewall, mapping of VRAM/doorbells, and a log. No PSP/SMU code; dword 0x16000-0x17FFF
  (MP0, MP1, THM, SMUIO, CLK, FUSE) and the SMN/MM_INDEX ways to the same registers are refused. Read-only until armed.
- `include/ps5gpu_ioctl.h`: the interface.
- `tool/ps5gpu.c`: `ps5gpu.exe` (`info`, `probe`, `rd`, `vmtest`). The GPU bring-up logic will live in user mode on top
  of the shim, so a mistake ends a process and not Windows.
- `tool/stage2.cpp`: `ps5gpu2.exe`, the user-mode bring-up (`look`, `run <fwdir> [1-8]`, `libtest N`, `ringdump`,
  `kick`). `tool/layout.h`: where things are in VRAM. `tool/backend_win.cpp` / `backend_mock.cpp`: the console, or a
  register/memory model with a small CP simulator for testing on the Mac (`./build-mock.sh`).
- `lib/ps5drm.{h,cpp}`: libps5gpu, the kernel-driver equivalent for a client program (buffers, address space,
  submission, fences). Ring discipline is Linux's: 2048-dword ring, 256-dword blocks, no packet across the ring end.
- `wddm/ps5wddm.cpp`: the display driver (display-only or full WDDM 1.3). `wddm/gpuboot.{h,cpp}`: the GPU start of
  `ps5gpu2 run` (steps 1-6, 8) without an OS underneath; ps5wddm runs it in DriverEntry at boot and then registers as
  the full driver at once (no blink). `./build-gpuboot-mock.sh` checks it write for write against stage2 on the model.
  Console side: `win/gpu-bootstart.cmd` (status / arm / on / off / test), `win/autostart.cmd` (fallback + arming).
- `linux-ref.py`: read-only register snapshot under Linux (kernel-mode reads through /dev/mem).
- `build.sh`: builds both with mingw-w64 (Homebrew), test-signs with a local certificate (`../build/ps5gpu/`).
  `ps5gpu-vm.sys` is the rehearsal variant: its "GPU" is QEMU's VGA device.
- Reference sources (not in this folder): `../build/gpusrc/linux` (Linux 7.2, AMD GPU part), `../build/gpusrc/mesa`
  (Mesa 26.3.1). Microcode files: the `cyan_skillfish2_*.bin` files from linux-firmware-amdgpu (the image ships them in `/usr/lib/firmware/amdgpu`; the installer copies them to `C:\ps5\gpu\fw`).

Loading on the console (needs test-signing, which is set and takes effect at the next restart):
`sc create ps5gpu type= kernel start= demand binPath= C:\ps5\ps5gpu.sys`, `sc start ps5gpu`, `C:\ps5\ps5gpu.exe probe`.
