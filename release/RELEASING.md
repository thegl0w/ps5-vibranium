# Releasing ps5win

The steps for one release, in order. Nothing is published before step 5.

## 1. Attachments (folder `build/release-attachments/`)

| File | What |
|---|---|
| `ps5-windows-installer.img.xz` | the image (built by `image/build-image.sh`, checked, then compressed with `xz -T0`) |
| `ps5-windows-installer.img.xz.minisig` | minisign signature of the `.xz` |
| `SHA256SUMS` | SHA-256 of every attachment and of the `.img` inside the `.xz` (`shasum -a 256 -c SHA256SUMS` checks the files; the `.img` line is checked after unpacking) |
| `SHA256SUMS.minisig` | minisign signature of `SHA256SUMS` |
| `linux-7.2.3.tar.xz` | Linux source for the shipped kernel (GPL-2.0), from kernel.org |
| `linux.patch` | ps5-linux-patches @ 5f6b5d32 (the PS5 patch the kernel was built with) |
| `kernel-7.2.3-p5f6b.config` | the kernel configuration (see `release/KERNEL-SOURCE.md`) |
| `vkd3d-proton-3.1-src.tar.xz` | vkd3d-proton v3.1 source incl. all submodules (LGPL-2.1; the DLLs ship unmodified; tag commit 4522cc1a) |

Before signing: the README's Download table carries the same two SHA-256 values as `SHA256SUMS`.

## 2. Signing (minisign)

Once, to create the key (the passphrase is the maintainer's; it is never written down anywhere in the project):

```bash
minisign -G -p release/ps5win.pub -s ~/.minisign/ps5win.key
```

Put the public key (the second line of `ps5win.pub`) into the README, section "Download". Keep `~/.minisign/ps5win.key`
off every drive that leaves the house except the backup drive.

For each release:

```bash
cd build/release-attachments
minisign -S -s ~/.minisign/ps5win.key -m ps5-windows-installer.img.xz SHA256SUMS   # -s before -m: -m takes every later word as a file
minisign -V -p ../../release/ps5win.pub -m ps5-windows-installer.img.xz && minisign -V -p ../../release/ps5win.pub -m SHA256SUMS
```

## 3. Final checks on the exact files

- Fresh install from the exact `.img.xz` onto a fresh drive with nothing from the dev setup attached; second start
  reaches full mode (green self-test square, one blink, "PS5 GPU (ps5wddm)").
- Privacy sweep of the export (`release/export.py`) and of the image's own content.
- `tools/clonecheck.py` and `tools/origins.py` re-run on the final export; `docs/CLONECHECK.md` and `docs/ORIGINS.md`
  regenerated.

## 4. Repository

- Create the repository from `build/public-export` (exported by `release/export.py`).
- Replace `REPO_URL` in `.github/ISSUE_TEMPLATE/config.yml` with the repository's URL.
- Settings: enable **Private vulnerability reporting** (Security) and **Discussions**.
- Open the first issue with the text of `docs/PINNED-ISSUE.md` and pin it.

## 5. Publish

Create the GitHub release with the attachments from step 1. Do not announce it yet.

## 6. Download-back check (before announcing)

From a clean folder, download every attachment back from the release page (not from the local copies), then:

```bash
shasum -a 256 -c SHA256SUMS                                          # the .xz matches
minisign -V -p ps5win.pub -m ps5-windows-installer.img.xz              # signed by our key
minisign -V -p ps5win.pub -m SHA256SUMS
xz -dc ps5-windows-installer.img.xz | shasum -a 256                  # = the .img line in SHA256SUMS
```

Compare each value with the README's Download table. Only when all four agree, announce the release.

## 7. Backup

Copy `build/public-export`, `build/release-attachments` and the minisign key to the backup drive (never the PS5 test
drive), and check the copy with `shasum -a 256 -c SHA256SUMS` on the drive.
