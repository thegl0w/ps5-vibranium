# PS5 Windows installer (work in progress)

Design: `DESIGN.md`. Progress: `NOTES.md`. Only `install.py` without `--dry-run` writes to a disk, and only on the PS5's Linux as root, after the typed
confirmation; it has not been run yet. Everything else is read-only or writes into a scratch directory.

## Scripts

| Script | What it does | Writes |
|---|---|---|
| `autorun` / `autorun.py` | The automatic run on the installer disk (tty1): ISO on PS5ISO → target = its own disk (append) → dry run → exact disk on screen + 30 s countdown (any key cancels) → real run → logs on PS5ISO → starts Windows. `--dry-run --fixture` = test mode | real: the boot disk's free space; test: work dir only |
| `make_image_tree.py OUTDIR` | Assembles `/opt/ps5win/installer/` for the image session: the tools, `deny.txt`, `kstub-table.txt` from the launch package (default build/boot33), the payload, `TREE.txt` | only OUTDIR |
| `install.py` | The whole install. `--dry-run` (anywhere): every step except the disk writes, then the WOULD RUN list. Without it (PS5 Linux, root, `--live` or `--image-file NEW.img`): the same steps, the typed confirmation (or answer file), then `lib/writer.py` writes, each write behind a fresh check of the target | `--dry-run`: only `--workdir`; real: the confirmed target |
| `find_iso.py PATH...` | Finds Windows ISOs (or copied ISO folders), reads `sources/install.wim` from the ISO in place (UDF parser in `lib/udf.py`), lists the images, picks IoT Enterprise LTSC x64 10.0.19041 | nothing |
| `find_iso.py --block-devices` | PS5 Linux only: mounts each USB partition read-only and scans it | read-only mounts under `/run/ps5inst/scan` (not run yet) |
| `check_kernel.py WIM INDEX` | ntoskrnl TimeDateStamp/SizeOfImage of the image vs. `firmware/windows_kstub*.inc` | temp dir |
| `plan_layout.py` | Target disk plan: refusals, partitions, PARTUUIDs, the commands a real run would execute, the typed confirmation | nothing |
| `offline_reg.py WIM INDEX DIR` | Extracts SYSTEM/SOFTWARE of the image into DIR and applies the pre-first-start registry edits value by value | only DIR |
| `build_payload.py OUTDIR` | Collects `C:\ps5` from the build outputs (`payload/manifest.txt`), checks drivers (signature, flags), catalogs, DLL bitness, Vulkan manifests, CRLF, every `C:\ps5` path the scripts name, and that no file holds the user's network details (values never printed); writes `MANIFEST.sha256`; `--frozen <payload.json>` refuses any file that differs from an earlier (release) build. Refuses an OUTDIR inside the repo | only OUTDIR |
| `lib/writer.py` | The write sequence: GPT backup, sgdisk with the plan's exact sectors, PARTUUID/geometry check, mkfs.vfat, mkntfs, one-pass NTFS-mode apply, ntfs3 **read-only** check, PS5BOOT copy + hash check, `/etc/ps5win.conf`. Before every write: target looked up again (serial, path, size, USB, not the internal SSD, nothing mounted / kept partitions unmoved); any change stops before that write. No rollback | — |
| `make_bcd.py` | Writes the PS5BOOT BCD with libhivex (boot manager on PS5BOOT, one Windows entry on PS5WIN, PS5 settings); `--dump` lists any BCD store with element names and a structure check | only `--out` |
| `make_unattend.py --out FILE` | Generates `unattend.xml` (no OOBE screens, local admin with autologon, first-logon script); optional answer-file keys; time zone from `/etc/localtime` or the answers, checked against the image (`--hive`); never prints the password or product key | only FILE |
| `payload/setup/` | `SetupComplete.cmd` → `ps5-setup.cmd` (first start, SYSTEM), `wu-tasks-off.ps1`, `firstlogon.cmd` | — (run on Windows) |
| `tests/overlay_test.py` | Proves the one-pass apply: in-memory overlay on the read-only install.wim, then extraction sees our files | only the scratch dir |
| `lib/wimapi.py`, `lib/hive.py` | ctypes bindings to libwim and libhivex | — |

## Optional settings (PS5ISO/ps5win-answers.txt)

`ps5win-answers.example.txt` lists every key the installer reads from PS5ISO (computer name, account, password,
time zone, locale, product key, Windows partition size), all commented out with their defaults. Copy it to PS5ISO as
`ps5win-answers.txt` and uncomment what you want. The image carries a copy at
`/opt/ps5win/installer/ps5win-answers.example.txt`.

## Known limits of an installed system

- **Windows Update is off on purpose** (the firmware's kernel stub is tied to the ISO's ntoskrnl 19041.1288; a new
  kernel would boot without the per-CPU fix). Delivery Optimization is off too.
- **Microsoft Defender** starts with the ISO's antivirus signatures (1.303.25.0, 2019) and updates them itself over
  the network even with Windows Update off (seen on a fresh install over Ethernet, 2026-11-22). Without a network
  connection it stays on the old definitions.
- **macOS cannot mount PS5BOOT** (the small FAT32 boot partition): it reports the volume as damaged, even read-only.
  Windows, Linux and the PS5 firmware use it normally; read it from Linux if needed.
- **Anti-cheat:** don't play online games with anti-cheat (EasyAntiCheat, BattlEye, Vanguard) on this install;
  modified graphics files can get accounts banned. The game kit skips such games. The installer's countdown screen
  and `READ ME FIRST.txt` on the desktop say so too.
- **Ethernet:** connect the cable before starting Windows; plugging it in later is not picked up yet (no hot-plug).
- **PS5 OS** says the console "wasn't turned off properly" after a Windows session: expected (Windows' restart and
  shut-down go through the console's own reset path, not PS5 OS).
- **HDMI audio** works from the first restart on (its interrupt setting applies at the next boot).
- `C:\ps5\security-report.txt` (written at the first start) shows what the install exposes: accounts, listening
  ports, firewall, Remote Desktop, SSH, file sharing, Defender and update status.

## Requirements (Mac)

Homebrew `wimlib` and `hivex` (both already installed here: `/opt/homebrew/lib/libwim.dylib`,
`/opt/homebrew/lib/libhivex.dylib`), Python 3. On Linux the same libraries are found by name
(`LIBWIM=` / `LIBHIVEX=` override the path).

## Running the tests

```bash
bash tests/run_tests.sh
```
Layout planner, block-device filter and payload (fixtures in `tests/fixtures/`; payload tests need this Mac's
`build/` outputs), 36 checks.

```bash
bash tests/run_tests.sh /Volumes/CES_X64FREO_EN-US_DV9
```
All 59 checks: also the ISO pick, kernel-stub match, offline registry edits and the overlay proof (with a
placeholder tree and with the real payload). Needs the ISO
mounted (double-click it in Finder; the folder name is its volume label). Hive copies go to a new temp dir, or to
`SCRATCH=/some/dir`. Passing the `.iso` file instead of the folder runs the ISO pick on the file (UDF reader) and
skips the wimlib-based tests.

## The installer disk's automatic run, in test mode

```bash
python3 make_image_tree.py /tmp/imgtree && /tmp/imgtree/installer/autorun --dry-run --fixture tests/fixtures/toshiba-image.json --media /Volumes/CES_X64FREO_EN-US_DV9 --payload /tmp/imgtree/installer/payload --firmware-dir <folder with the cyan_skillfish2_*.bin files from linux-firmware-amdgpu> --countdown 0 --workdir /tmp/ar/work
```

## Trying the whole dry run by hand

```bash
python3 install.py --dry-run --answers tests/fixtures/answers-install.txt --media /Volumes/CES_X64FREO_EN-US_DV9 --fixture tests/fixtures/two-disk.json --payload-from-build --firmware-dir <folder with the cyan_skillfish2_*.bin files from linux-firmware-amdgpu>
```
About 5 s on this Mac. The microcode folder here is only for the rehearsal; on the console it is Linux's own
`/usr/lib/firmware/amdgpu`.

## Trying the planner by hand

```bash
python3 plan_layout.py --fixture tests/fixtures/two-disk.json --target-serial T7TARGET0002 --mode erase
```
Shows the target disk, what is erased or kept, the new partitions, the exact `sgdisk`/`mkfs`/apply commands
(marked WOULD RUN) and the phrase a real run would wait for. Fixtures: `two-disk.json` (Linux SSD, empty T7,
ISO stick, the internal NVMe, and the internal SSD disguised as a USB disk), `single-disk.json` (Linux image on a
500 GB SSD, rest free), `small-disk.json`, `installed-disk.json`.

On the console (later, when it is free): `--live` reads `lsblk` and `/sys` read-only instead of a fixture.

## Proposals for files this session does not own

`proposals/firmware-launcher.md`: patches for `firmware/` and the launch package's `run_stage.sh` (kernel-stub table,
stop on a missing stub, PS5BOOT by name, `/etc/ps5win.conf`). Check offline with
`bash proposals/tests/kstub_test.sh` (applies them to a temp copy; never to `firmware/` or `build/`).
