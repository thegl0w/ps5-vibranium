#!/bin/bash
# SPDX-License-Identifier: MIT
# Runs inside the x86_64 Arch container. Turns /build/root (Arch base from base-inside.sh) into the Toshiba image:
#   p1 PS5 Linux boot (FAT32 512 MiB, label boot, GPT name PS5LINUX): bzImage, initrd.img, cmdline.txt, vram.txt (+ the loader's files)
#   p2 PS5ISO (exFAT 10 GiB, empty): the user copies the Windows ISO here from the Mac; logs land here too
#   p3 ps5arch (ext4 10 GiB): Arch + /opt/ps5win (launcher, launch package, installer)
#   after p3: free space for the installer (PS5BOOT + PS5WIN on the same disk)
# Inputs (read-only): /image/overlay (our files), /in/kernel (bzImage + modules/<ver> + firmware/amdgpu from the console),
# /in/package/deploy.tgz (launch package), /in/installer (installer session's tree, optional until it is ready).
# Output: /out/ps5-windows-installer.img (sparse) + .sha256
set -euo pipefail
sed -i -e 's/^#DisableSandbox/DisableSandbox/' -e 's/^DownloadUser/#DownloadUser/' /etc/pacman.conf
# PACMAN_MIRROR=<url>: one mirror for this build (2026-11-22: the container default fastly.mirror.pkgbuild.com
# timed out twice in a row).
[ -n "${PACMAN_MIRROR:-}" ] && echo "Server = ${PACMAN_MIRROR}/\$repo/os/\$arch" > /etc/pacman.d/mirrorlist
pacman -Syu --noconfirm --needed gptfdisk dosfstools exfatprogs e2fsprogs mtools rsync diffutils >/dev/null

R=/build/root
kver=$(ls /in/kernel/modules)
test -f /in/kernel/bzImage && test -d /in/kernel/modules/$kver && test -d /in/kernel/firmware/amdgpu && test -f /in/kernel/vram.txt
test -f /in/package/deploy.tgz

# --- the Arch system ---
rsync -a /image/overlay/ $R/
# firmware the console's other devices ask for (MT7961 Wi-Fi/Bluetooth): no endless retries on the console
# earlier builds copied the console's microcode in by hand: remove those unowned copies so the package can own them
pacman --root $R -Q linux-firmware-amdgpu >/dev/null 2>&1 || rm -f $R/usr/lib/firmware/amdgpu/cyan_skillfish2_*
pacman --root $R --noconfirm --needed -Sy linux-firmware-mediatek linux-firmware-amdgpu >/dev/null
# GPU microcode from Arch's published package (ships LICENSE.amdgpu). It must stay byte-identical to what the console's
# Linux uses (the Windows payload copies it at install time): compare with the collected files, stop on any difference.
for f in /in/kernel/firmware/amdgpu/*; do cmp "$f" "$R/usr/lib/firmware/amdgpu/${f##*/}" || { echo "microcode differs: ${f##*/}"; exit 1; }; done
test -f $R/usr/share/licenses/linux-firmware-amdgpu/LICENSE.amdgpu
mkdir -p $R/usr/lib/modules $R/usr/lib/firmware/amdgpu $R/opt/ps5win/package
rsync -a --delete /in/kernel/modules/$kver/ $R/usr/lib/modules/$kver/
cp /in/package/deploy.tgz $R/opt/ps5win/package/
if [ -d /in/installer ]; then rsync -a --delete /in/installer/ $R/opt/ps5win/installer/; fi
echo ps5win > $R/etc/hostname
: > $R/etc/machine-id; rm -f $R/var/lib/dbus/machine-id
# an empty machine-id means "first boot": systemd-firstboot would then prompt on the console (locale, root password).
# Nobody types on the console: mask it and set the locale it would ask for.
ln -sf /dev/null $R/etc/systemd/system/systemd-firstboot.service
echo LANG=C.UTF-8 > $R/etc/locale.conf
printf 'LABEL=ps5arch / ext4 rw,relatime 0 1\nPARTLABEL=PS5LINUX /boot/ps5 vfat rw,noauto,nofail 0 2\nLABEL=PS5ISO /mnt/ps5iso exfat rw,nofail 0 0\n' > $R/etc/fstab
mkdir -p $R/boot/ps5 $R/mnt/ps5iso
printf 'KEYMAP=us\nFONT=ter-v32b\n' > $R/etc/vconsole.conf
ln -sf "/usr/share/zoneinfo/${IMG_TZ:-UTC}" $R/etc/localtime
test -e $R/etc/localtime
# initramfs: no autodetect (the build container is not the console); the storage and USB modules the console's own
# initramfs carries (collected list in /in/kernel/initrd-modules.txt) are added explicitly
printf 'MODULES=(amdgpu)\nBINARIES=()\nFILES=()\nHOOKS=(base udev modconf kms keyboard block filesystems fsck)\nCOMPRESSION="zstd"\n' > $R/etc/mkinitcpio.conf
mount -t proc proc $R/proc; mount -t sysfs sys $R/sys; mount --rbind /dev $R/dev; mount -t tmpfs tmp $R/run
trap 'umount -R $R/run $R/dev $R/sys $R/proc 2>/dev/null || true' EXIT
chroot $R systemctl enable ps5win-autostart.service >/dev/null
chroot $R depmod "$kver"
# initramfs for the PS5 kernel (root found by label)
chroot $R mkinitcpio -k "$kver" -g /boot/initramfs-ps5.img >/build/mkinitcpio.log 2>&1 || { tail -20 /build/mkinitcpio.log; exit 1; }

umount -R $R/run $R/dev $R/sys $R/proc; trap - EXIT
rm -rf $R/var/cache/pacman/pkg/* $R/var/lib/pacman/sync/*

# --- partition images ---
W=/build/work; rm -rf $W; mkdir -p $W
truncate -s 512M $W/p1.img; mkfs.fat -F 32 -n boot $W/p1.img >/dev/null 2>&1
mcopy -i $W/p1.img /in/kernel/bzImage ::bzImage
mcopy -i $W/p1.img $R/boot/initramfs-ps5.img ::initrd.img
printf 'root=LABEL=ps5arch rw rootwait rootdelay=60 usbcore.autosuspend=-1 console=ttyTitania0 console=tty0 video=DP-1:1920x1080@60 mitigations=off idle=halt preempt=full usbc_dp.enable=0 crashkernel=400M quiet loglevel=3\n' > $W/cmdline.txt
cp /in/kernel/vram.txt $W/vram.txt
mcopy -i $W/p1.img $W/cmdline.txt ::cmdline.txt
mcopy -i $W/p1.img $W/vram.txt ::vram.txt
if [ -d /in/kernel/bootfiles ]; then for f in /in/kernel/bootfiles/*; do mcopy -o -i $W/p1.img "$f" ::; done; fi
truncate -s 10G $W/p2.img; mkfs.exfat -L PS5ISO $W/p2.img >/dev/null
truncate -s 10G $W/p3.img; mke2fs -q -t ext4 -L ps5arch -d $R $W/p3.img

# --- the disk image ---
mkdir -p /out; img=/out/ps5-windows-installer.img; rm -f $img
s1=$(( $(stat -c %s $W/p1.img) / 512 )); s2=$(( $(stat -c %s $W/p2.img) / 512 )); s3=$(( $(stat -c %s $W/p3.img) / 512 ))
start1=2048; start2=$((start1 + s1)); start3=$((start2 + s2)); end=$((start3 + s3))
truncate -s $(( (end + 2048) * 512 )) $img
sgdisk -o -n 1:$start1:$((start2 - 1)) -t 1:0700 -c 1:PS5LINUX \
          -n 2:$start2:$((start3 - 1)) -t 2:0700 -c 2:PS5ISO \
          -n 3:$start3:$((end - 1)) -t 3:8300 -c 3:ps5arch $img >/dev/null
dd if=$W/p1.img of=$img bs=1M seek=$((start1 * 512 / 1048576)) conv=notrunc,sparse status=none
dd if=$W/p2.img of=$img bs=1M seek=$((start2 * 512 / 1048576)) conv=notrunc,sparse status=none
dd if=$W/p3.img of=$img bs=1M seek=$((start3 * 512 / 1048576)) conv=notrunc,sparse status=none
sgdisk -p $img
ls -lsh $img
