#!/bin/bash
# step 1: check + mount the ext4 root read-only, manifest it
set -u
. /tmp/m-common.sh
blkid "$dev"
e2fsck -f -p "$dev"; r=$?; echo "e2fsck exit $r"; [ $r -le 1 ] || exit 1
mkdir -p /mnt/old && mount -o ro "$dev" /mnt/old || exit 1
df -h /mnt/old
manifest /mnt/old /tmp/m-old
