# Linux kernel in ps5win: corresponding source (GPL-2.0)

ps5win ships one Linux kernel build, used twice:

- the image's Linux (`boot` partition, `bzImage`), and
- the launch package's stage kernel (`windows.bzImage`), which is the same kernel binary wrapped with a small stub
  from ps5-hackintosh's `bringup/` (MIT).

Kernel: **Linux 7.2.3** with the PS5 patch **ps5-linux-patches @ 5f6b5d32ab5d8f162ac4af14d00fdbabacd94ea6**
(`uname -r` = `7.2.3-p5f6b`, built with GCC 16.2.1).

| Part | Where |
|---|---|
| Linux 7.2.3 | https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz *(attached to the release)* |
| PS5 patch | https://github.com/ps5-linux/ps5-linux-patches/blob/5f6b5d32ab5d8f162ac4af14d00fdbabacd94ea6/linux.patch *(attached)* |
| Configuration | `kernel-7.2.3-p5f6b.config` *(attached)* |

## About the configuration

`kernel-7.2.3-p5f6b.config` is the configuration saved next to this kernel on the console it was built on. Its options
match the shipped binary: the one option where it differs from the patch repository's own `.config` at that commit is
`CONFIG_CEC_CORE`, which is built in (`=y`) here and in the shipped kernel (`cec.ko` is listed in `modules.builtin`).
Compiler-derived values (`CONFIG_CC_VERSION_TEXT`, `CONFIG_GCC_VERSION`, `CONFIG_AS_VERSION`, Rust/pahole versions) are
recomputed by `make olddefconfig` for whichever toolchain builds it.

## Build

```bash
tar xf linux-7.2.3.tar.xz && cd linux-7.2.3
patch -p1 < ../linux.patch
cp ../kernel-7.2.3-p5f6b.config .config
make olddefconfig
make LOCALVERSION=-p5f6b -j"$(nproc)" bzImage modules
```
