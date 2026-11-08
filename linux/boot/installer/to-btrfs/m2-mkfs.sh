#!/bin/bash
# step 2 (after the backup is verified on the PC): btrfs with @ (default) and @home
set -eu
. /tmp/m-common.sh
umount /mnt/old
wipefs -a "$dev"
mkfs.btrfs -f -L arch-root "$dev"
mkdir -p /mnt/top /mnt/root
mount -o noatime,compress=zstd:3 "$dev" /mnt/top
btrfs subvolume create /mnt/top/@
btrfs subvolume create /mnt/top/@home
btrfs subvolume set-default /mnt/top/@
umount /mnt/top
mount -o noatime,compress=zstd:3 "$dev" /mnt/root
mkdir /mnt/root/home
mount -o noatime,compress=zstd:3,subvol=/@home "$dev" /mnt/root/home
btrfs subvolume list /mnt/root; btrfs subvolume get-default /mnt/root; findmnt -R /mnt/root
