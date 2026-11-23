# ps5win: Windows 10 on the PlayStation 5, with a GPU driver

> **Beta.** This is the first public release. Expect rough edges, and please report what you find.

> **Built with AI assistance.** Much of the code and documentation was written with Claude (Claude Code), directed,
> reviewed and tested on real hardware by the author.

ps5win installs **Windows 10 IoT Enterprise LTSC 2021** on a launch-model PS5 and runs it bare metal from a USB drive.
Its own GPU driver gives Windows a GPU-accelerated desktop, Vulkan, and Direct3D 9 to 12 games through DXVK and
vkd3d-proton.

**Demo:** https://youtu.be/ygxBbsQypKQ (3 minutes, firmware 13.60, boot to desktop with the ps5wddm GPU driver).
Gameplay and a full-install video will follow.

You write one image to a USB drive and copy **your own** Windows ISO onto it. Then you start the PS5 Linux loader as usual,
and the drive installs Windows by itself and starts it. Nothing on the PS5's internal storage is touched. To go back to
plain PS5 use, shut Windows down and unplug the drive. There is nothing to uninstall.

No Windows files are included or downloaded. You bring the ISO, and you need a valid licence for Windows 10 IoT
Enterprise LTSC 2021.

---

## What works

| Area | State |
|---|---|
| Install | Fully automatic from the USB drive: finds the ISO, shows the target disk, 30-second countdown (any key cancels), installs, starts Windows. Logs are written to the drive. |
| Boot | The PS5 Linux loader, then Linux, then our UEFI firmware, then Windows. After the install, a 10-second countdown starts Windows by itself; any key stays in Linux. |
| Display | Our WDDM 1.3 kernel driver (`ps5wddm`). The Windows compositor (dwm.exe) renders on the GPU. The screen's own resolution at **60 Hz** (tested at 1920×1080 and 2560×1440; 4K is untested). Optional line in `ps5win-display.txt` on PS5ISO: `mode=2560x1440` picks another resolution the screen offers. (120 Hz works in development and comes in a later release.) |
| Vulkan | Mesa RADV, ported to Windows on our own kernel interface (`libps5gpu`). 64-bit and 32-bit. |
| Direct3D 10/11 (system) | Mesa's D3D10 user-mode driver on Zink on RADV. Runs the desktop and ordinary programs. |
| Games | A game kit adds DXVK (D3D9/10/11) and vkd3d-proton (D3D12) next to a game's exe, automatically within seconds of the game appearing, or from the right-click menu. Tested in a window (162 fps, heavy shader) and full screen with direct scan-out (60 fps, vsync). |
| GPU robustness | A program that dies mid-frame can't stall the GPU for everyone. A watchdog restarts a hung GPU without restarting Windows. |
| Network | Ethernet, with our own driver (`p5eth.sys`), **experimental**: the link comes up, but in our latest tests Windows didn't always get an address by DHCP. Connect the cable before starting; hot-plug comes later. A USB network adapter is the alternative. |
| Storage | USB drives. Other USB drives plugged in while Windows runs get a drive letter as usual. The internal NVMe is blocked on purpose (see Safety). |

## What doesn't work (yet)

- **Tested screens:** 1920×1080 and 2560×1440, at 60 Hz. 120 Hz isn't in this release yet.
- **One resolution per boot.** Windows runs at the resolution chosen at start (the screen's preferred one, or `mode=` in `ps5win-display.txt`); it can't be changed from Windows' display settings. Games that ask for a lower full-screen mode get none.
- **Launch-model PS5 only.** Tested on the original (launch) PS5. The PS5 Slim is untested (reports welcome); the PS5 Pro is not supported.
- **USB drive only; no M.2 install.** Windows and its launcher live on the USB drive you write the image to. Installing to an M.2 SSD in the PS5's expansion slot isn't supported: the PS5 formats that drive with its own layout (partitions start in the middle of the disk), which neither our firmware nor Windows can boot from yet. Don't run the ps5-linux `m2_init` / `m2_install.sh` tools with this image; they only move the Linux part and Windows would still need the USB drive.
- **4K is untested.** The PS5 Linux kernel only offers Sony's standard HDMI modes (1080p60, 1080p120, 1440p, 4K60 in full RGB). A screen that offers 4K only at 30 Hz or only in 4:2:0 (common on capture cards) gets 1080p or 1440p instead.
- **32-bit Direct3D 10/11 through Windows' own runtime:** no hardware device yet. 32-bit games through DXVK work.
- **DirectDraw / Direct3D 7 and older games:** not supported yet. A dgVoodoo2-based path is planned.
- **Wi-Fi:** our driver works, but its current build is tied to the test console and can't ship. A clean build comes
  later; until then use Ethernet or a USB network adapter.
- **The Blu-ray drive and Bluetooth:** not yet.
- **Audio** works over HDMI ("Digital Audio (HDMI)") from the first restart after the install on.
- **Windows Update is off** (see Security).
- **Anti-cheat games:** not supported, and not planned.
- **Mesa's disk shader cache** is off on Windows. DXVK and vkd3d-proton keep their own caches.
- **Edge** doesn't use the GPU yet.

## Requirements

- **A launch-model PS5** (disc or digital) that can boot the PS5 Linux loader.
  Tested on **13.60**.
- **A USB drive of at least 128 GB**, plugged **directly** into a PS5 port, not through a hub or dock. The image uses
  about 21 GB and Windows needs at least 40 GB more, plus room for games. An SSD is much faster than a hard drive.
  The drive must report 512-byte sectors, as most do. A few USB adapters report 4096-byte sectors; the installer
  refuses those before writing anything.
- **The tested Windows ISO.** Our firmware patches the Windows kernel for the PS5's CPU, and the patch is built for
  exact ntoskrnl builds. Use this ISO:

  | | |
  |---|---|
  | File | `en-us_windows_10_iot_enterprise_ltsc_2021_x64_dvd_257ad90f.iso` |
  | SHA-256 | `a0334f31ea7a3e6932b9ad7206608248f0bd40698bfb8fc65f14fc5e4976c160` (the copy we tested; Microsoft doesn't publish a hash for this ISO publicly) |
  | Windows | 10 IoT Enterprise LTSC 2021, x64, build 19044.1288 (ntoskrnl 10.0.19041.1288) |

  Check yours with `shasum -a 256 <file>` (macOS/Linux) or `Get-FileHash <file>` (Windows). The installer checks the
  edition and kernel build before writing anything, and refuses anything else with a message. **Only this ISO is
  reported as working.** Other editions and languages of the same build (19044.1288) are planned but untested.
- A computer to write the image (macOS, Linux or Windows).
- A USB keyboard and mouse.

## Download

Download `ps5-windows-installer.img.xz` (386 MB, 20.5 GiB once unpacked) from this repository's **Releases** page;
the release date is shown there. Check its SHA-256 before writing it:

| File | SHA-256 |
|---|---|
| `ps5-windows-installer.img.xz` | `c74971a182fcb0f00bc066ff59c21d415a1dc9eda01cd26ab46a47e11be8b914` |
| `ps5-windows-installer.img` (inside) | `6e0ff643ad0b36ca10e4865cf7344b22b24e5ef93bc0d29c92db865b19d9b73b` |

The release files are signed with [minisign](https://jedisct1.github.io/minisign/). Our public key (also in
`release/ps5win.pub`):

```
RWTZPJHamtTC2i/7gqe8aeGcdAw57C0um+zEmx4QkmRcrkhBmIZ8j8G0
```

Check a download with `minisign -V -P RWTZPJHamtTC2i/7gqe8aeGcdAw57C0um+zEmx4QkmRcrkhBmIZ8j8G0 -m ps5-windows-installer.img.xz`
(the `.minisig` file next to it). A file that doesn't verify is not from this project: don't write it to a drive.

## Install

1. **Write the image to the USB drive.** balenaEtcher or Raspberry Pi Imager is easiest; both write the `.xz`
   directly. With `dd`: `xzcat ps5-windows-installer.img.xz | sudo dd of=/dev/<disk> bs=4M`.
   **Check the drive you picked: writing erases it, and choosing the wrong disk erases that one.**
2. **Copy the ISO** onto the drive's `PS5ISO` partition (exFAT, readable on any computer). The name doesn't matter.
3. **Plug the drive into the PS5** and unplug every other USB drive.
4. **Start the PS5 Linux loader** the way its own project describes. The loader
   boots the drive's own small Arch Linux.
5. **Wait.** The installer finds the ISO, shows the exact disk it will use, and counts down 30 seconds. Press any key
   to cancel. Otherwise it installs into the drive's free space and starts Windows. On the first start, Windows sets
   itself up and installs the PS5 drivers with no screens to click through.

**Once you reach the desktop the first time, restart one more time** (Start → Restart, then start the PS5 Linux loader again). Audio and the full GPU driver only come up from that second start on; on the first desktop, sound may show a red cross and Display Settings may say "Microsoft Basic Display Adapter". That's expected. On the second start, shortly after the desktop appears, a green rectangle shows briefly near the top left (the GPU checking that it can draw) and then the screen goes black once for about a second: that is the switch to the full GPU driver. After it, Display Settings → Advanced display says "PS5 GPU (ps5wddm)".

On every later start: start the PS5 Linux loader, and Windows comes up after a 10-second countdown. Press any key to stay in Linux.

### Optional: answer file

Put `ps5win-answers.txt` on `PS5ISO` before the install to set the user name, computer name, time zone and similar.
Copy `installer/ps5win-answers.example.txt` (also in the image at `/opt/ps5win/installer/`) to the `PS5ISO` partition as `ps5win-answers.txt` and uncomment what you want.

**Don't put a password in it yet.** The `PS5ISO` partition is readable on any computer, and the file stays there after
the install. Set your password in Windows after the install instead: press Ctrl+Alt+Del, then *Change a password*.
*(Planned: the installer blanks the password line once it has used it; this note changes then.)*

## Security: read this

Your Windows here is less locked down than a normal install:

- **Test signing is on.** Our drivers are signed with our own test certificate, so Windows runs in test-signing mode,
  which lets the kernel load test-signed drivers. The certificate (`CN=PS5GPU Test Signing`, self-signed, RSA 2048)
  is added to the *Trusted Root* and *Trusted Publishers* stores. It is limited to code signing (extended key usage
  Code Signing only, `CA:FALSE`), so it can't vouch for websites or issue other certificates. Its private key is not
  part of ps5win's downloads.
- **Microsoft Defender** stays on and keeps its virus definitions up to date by itself when the console is online
  (Ethernet), even with Windows Update off: on a fresh install it fetched current definitions within about 20 minutes.
- **Windows Update is off.** The firmware's kernel patch must match the exact ntoskrnl build, so an update could stop
  Windows from booting. Fixes and updates come through ps5win releases.
- **The default account `ps5` has no password and logs on automatically.** Windows refuses network logons to accounts
  with an empty password, and neither Windows nor the Linux side runs an SSH server or any other remote login.
  Anyone at the console can still use it. Set a password in Windows after the install (Ctrl+Alt+Del, then
  *Change a password*).
- The Linux side logs `root` on automatically on the console screen only. It has no network login.

## Safety

- **The PS5's internal NVMe is never written.** It holds PS5 OS and your games. It is blocked three ways: the firmware
  hides it before Windows starts, Windows has a device-install deny rule for it (`PCI\VEN_104D&DEV_90EB`), and the
  installer refuses any disk that isn't the drive it booted from.
- No access to the PSP (security processor). No power-management messages beyond what Linux sends. No GPU bus reset.
- The installer writes only to its own drive's free space. It shows the exact disk and counts down before every
  write.
- Give the console room to breathe. Games load the GPU as hard as PS5 games do, and the PS5's own fan control applies.
- Use it at your own risk. This is experimental software on hardware nobody designed it for.

## Troubleshooting

| What you see | What it means |
|---|---|
| The installer stops before writing; its log says the kernel build isn't supported | Your ISO's Windows kernel build has no patch yet. Nothing was written. Use the tested ISO above. |
| Red screen before Windows saying "STOPPED: this Windows kernel has no kernel stub" | Windows' kernel changed after the install (for example an update was forced on) and the firmware has no patch for it; the reason is printed under the message. Nothing was patched. Restore the kernel or reinstall. |
| Nothing on screen after the loader starts | Plug the drive straight into a PS5 port (no hub or dock) with no other USB drives attached, and check the display cable. |
| A blue screen (":(") while Windows sets itself up after the install | Restart the console and start the PS5 Linux loader again: Windows setup continues where it stopped (it may offer a disk check first; that's fine). If it keeps happening at the same point, check `logs/` on the `PS5ISO` partition. |
| The installer stops or cancels | Read `logs/` on the `PS5ISO` partition from another computer. |
| Blue screen `INACCESSIBLE_BOOT_DEVICE` (second parameter `C0000034`), Windows freezes at the start, or the Linux loader can't find its files | The console's USB sometimes doesn't bring the drive up properly after a restart (a known, occasional PS5 USB fault, not caused by ps5win). Power the console off, unplug the drive and plug it back in, then start again. Always use a port on the console itself, never a hub or dock. |
| Stuck in Linux after the install, or a `root@ps5win` prompt on tty1 | Linux couldn't hand over to Windows this time (a key was pressed during the countdown, or the screen wasn't detected properly, e.g. the display cable was moved). Nothing is broken: restart the console with the display connected and start the loader again. The reason is in `logs/` on the `PS5ISO` partition. |
| Ethernet says "Network cable unplugged" | Plug the cable in **before** you start the console: the Linux step brings the port up and the Windows driver takes it over. Plugging it in while Windows runs isn't supported yet; restart with the cable in. |
| PS5 OS says the console "wasn't turned off properly", or shows a storage check / repair screen before it starts | Expected and harmless. Windows (and Linux) power the console off through its power controller directly, so PS5 OS never runs its own shutdown. Clearing that note would mean writing to Sony's settings storage, which ps5win never does. |

## How it works

```
PS5 OS --> PS5 Linux loader --> Arch Linux (on the USB drive)
   --> crash-kexec into our UEFI firmware (EDK2 UefiPayloadPkg, PS5 memory map, kernel patch for this CPU)
   --> Windows boot manager (PS5BOOT partition) --> Windows 10 (PS5WIN partition)
```

| Part | What it is |
|---|---|
| `image/` | Builds the USB image: GPT with `boot` (FAT32, PS5 Linux loader files), `PS5ISO` (exFAT, empty) and `ps5arch` (ext4, Arch Linux and the installer). |
| `installer/` | The install: reads the ISO in place, applies `install.wim` with wimlib, edits the offline registry with hivex, writes the BCD, the unattend file and the first-start setup. |
| `firmware/` | The UEFI firmware bundle and launcher (`run_stage.sh`). The kernel patch is matched to the ntoskrnl build; the launcher stops with a red screen on an unknown build rather than booting without it. |
| `ps5gpu/wddm` | `ps5wddm`, the WDDM kernel display driver. |
| `ps5gpu/lib` | `libps5gpu`: GPU contexts (VMIDs), the shared ring with fences, robustness and the watchdog. |
| `ps5gpu/mesa` | Mesa's RADV and D3D10 driver back end for Windows on `libps5gpu`. |
| `ps5gpu/win/kit` | The game kit: automatic placement, right-click menu, removal. |
| `cleanroom/p5eth-mit` | The Ethernet driver `p5eth.sys` (MIT: a clean-room rewrite from a written description of the hardware; it replaced our first driver, which followed the GPL Linux `mts` driver). See `docs/CLEANROOM.md`. |

## Game kit

Most games just work once the kit is placed. The kit watches your game folders (Steam, Epic, GOG and others) and
copies the right DLLs next to a game's exe within seconds. You can also do it by hand:

- **Right-click a game's exe**, then *PS5: add game kit* or *PS5: remove game kit*.
- Folders listed in `skip.txt` are left alone.

## Building from source

The build scripts run on macOS (Apple silicon) with Docker and Homebrew's LLVM and mingw-w64; most also run on Linux.
They assume this repository's layout, with outputs and downloaded sources under `build/` (not part of the repository).
Drivers must be test-signed with a code-signing certificate of your own (`ps5gpu/build.sh` makes one).

| Part | How |
|---|---|
| GPU kernel shim and tools | `ps5gpu/build.sh` (mingw-w64): `ps5gpu.sys`, `ps5gpu.exe`, `ps5gpu2.exe` |
| Display driver | `ps5gpu/wddm/build.sh` (clang-cl + lld-link against Microsoft's WDK headers from NuGet in `build/wdk`). The catalog is made on Windows with PowerShell's `New-FileCatalog` over the `.sys` and `.inf`, then signed with `osslsigncode`. |
| Ethernet driver | `cleanroom/p5eth-mit/build.sh` |
| Mesa (RADV, Zink, D3D10 driver) | Mesa 26.3.1 plus `ps5gpu/mesa/mesa-ps5.patch`, configured with `ps5gpu/mesa/configure.sh` and `mingw-cross.txt` (meson cross build with mingw-w64) |
| UEFI firmware | EDK2 `edk2-stable202505` with ps5-hackintosh's UEFI package: `firmware/compile.sh` (EDK2 build in Docker), `firmware/make_bundle.py` (memory map and ACPI tables; needs your own console's ACPI tables, kept out of the repository), `firmware/build_stage.sh` (stage 1, from ps5-hackintosh's bring-up code and the PS5 kernel), `firmware/pack_package.sh` (the launch package) |
| Windows payload | `installer/build_payload.py <out> --frozen installer/payload/freeze-2026-11-23-r13.json` refuses any file whose hash differs from the release's |
| Installer tree and tests | `installer/make_image_tree.py <out> --package <launch package> --frozen <freeze>`; `installer/tests/run_tests.sh` |
| Image | `PS5WIN_PACKAGE=<launch package> image/build-image.sh` (Docker, Arch Linux; package versions as in `release/arch-packages.txt`). The PS5 kernel, modules and GPU firmware go into `image/collect/kernel`; see `release/KERNEL-SOURCE.md`. |

## Source code and licences

ps5win distributes binaries of these projects. Each keeps its own licence; the exact source versions we shipped and
our patches are linked here.

| Project | Licence | Where it is | Source |
|---|---|---|---|
| Linux kernel 7.2.3 + [ps5-linux-patches @5f6b5d32](https://github.com/ps5-linux/ps5-linux-patches/blob/5f6b5d32ab5d8f162ac4af14d00fdbabacd94ea6/linux.patch) | GPL-2.0 | the image (`boot`, `ps5arch`) and the launch package's stage kernel (same binary) | attached to the release: upstream source, that patch, `kernel-7.2.3-p5f6b.config`; see `release/KERNEL-SOURCE.md` |
| Arch Linux packages (incl. wimlib, hivex, systemd) | various, incl. GPL/LGPL | the image (`ps5arch`) | exact versions in `release/arch-packages.txt` (generated from the image); sources from the Arch Linux archive for those versions |
| wimlib | GPL-3.0-or-later / LGPL-3.0-or-later | installer | Arch package (version in `release/arch-packages.txt`), upstream wimlib.net; unmodified |
| hivex 1.3.24 (library only; the installer doesn't use its GPL tools) | LGPL-2.1 | installer | Arch package source |
| Vulkan loader 1.4.363 (`vulkan-1.dll`) | Apache-2.0 | Windows driver payload | upstream release |
| AMD GPU microcode (`cyan_skillfish2_*`) | `LICENSE.amdgpu` (redistributable, ships with it) | the image (Arch `linux-firmware-amdgpu` 20261019-1); copied into Windows at install | Arch package |
| EDK2 | BSD-2-Clause-Patent | firmware | upstream tag `edk2-stable202505` (6951dfe7); our changes are the `firmware/` sources |
| [ps5-hackintosh](https://github.com/rodri0x/ps5-hackintosh) (rodri0x) | MIT | launch package: stage stub in `windows.bzImage`, UEFI payload build base | upstream repository; licence in `release/licenses/` |
| Mesa (RADV, Zink, d3d10umd) | MIT | Windows driver payload | Mesa 26.3.1 (fec8830e); our changes: `ps5gpu/mesa/mesa-ps5.patch` |
| mingw-w64 14.0.0 winpthreads (`libwinpthread-1.dll`, needed by the Vulkan and OpenGL drivers) | MIT and BSD-3-Clause (winpthreads); ZPL-2.1 (mingw-w64 runtime) | Windows driver payload | unmodified toolchain DLL; [mingw-w64 v14.0.0 source](https://sourceforge.net/projects/mingw-w64/files/mingw-w64/mingw-w64-release/), licences in `release/licenses/` |
| DXVK 3.1.1 | zlib | game kit | unmodified upstream release (DLLs byte-identical); [v3.1.1 source](https://github.com/doitsujin/dxvk/tree/v3.1.1), licence in `release/licenses/` |
| vkd3d-proton 3.1 | LGPL-2.1-or-later | game kit | unmodified upstream release (DLLs byte-identical); [v3.1 source](https://github.com/HansKristian-Work/vkd3d-proton/tree/v3.1) (a source tarball with all submodules is attached to the release), COPYING/AUTHORS in `release/licenses/` |

Our own code: MIT (see `LICENSE`; every file carries an SPDX header). Three parts that first followed code under other
licences were rewritten clean-room from written hardware descriptions and are MIT too: the Ethernet driver (was
GPL-2.0), the firmware's restart/shutdown library and the GPU microcode loader. A clone detector compared every shipped source file with MetalCyan, ps5-hackintosh, Linux's amdgpu driver, the ps5-linux kernel patch and the GPL code we used; the results and their explanations are in `docs/CLEANROOM.md`. Licence texts:
`release/licenses/`.

## Related projects

**Related:** [Nagi](https://github.com/D-Ogi/amdgpu-wddm) is a source-available Windows (WDDM) driver stack for the
same GPU family on the AMD BC-250 board. ps5win is a separate project with its own driver, and it boots Windows on the
PS5 itself.

## Credits

Made by the gl0w (Hasham Tariq).

Credits required by the licences of code ps5win uses or ships:

- **[ps5-hackintosh](https://github.com/rodri0x/ps5-hackintosh)** by rodri0x (MIT): the launcher stub and the base of
  the UEFI firmware build (EDK2 payload setup, bundle maker).
- **Linux** with the **[ps5-linux](https://github.com/ps5-linux/ps5-linux-patches)** kernel patches (GPL-2.0): the
  image's kernel and the launch package's stage kernel; source and configuration in `release/KERNEL-SOURCE.md`.
- EDK2, Mesa, DXVK, vkd3d-proton, the Vulkan loader, wimlib, hivex, Arch Linux packages, AMD GPU microcode
  (`LICENSE.amdgpu`): see the licence table above and `release/licenses/`.

Not affiliated with Sony Interactive Entertainment or Microsoft. PlayStation and PS5 are trademarks of Sony
Interactive Entertainment. Windows is a trademark of Microsoft.

## Official source and forks

The only official source of ps5win is this repository. Builds, images or "installers" offered elsewhere may be modified; don't run them. If you fork ps5win and publish your own builds, please give them a different name so users can tell them apart.
