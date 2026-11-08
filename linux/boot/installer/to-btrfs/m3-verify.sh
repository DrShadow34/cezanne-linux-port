#!/bin/bash
# step 3 (after the restore): same manifest on the new tree, compare
set -u
. /tmp/m-common.sh
manifest /mnt/root /tmp/m-new
cd /tmp || exit 1
for f in list sums xattrs; do
	if cmp -s m-old/$f m-new/$f; then echo "$f: identical"; else echo "$f: DIFFERS"; diff m-old/$f m-new/$f | head -20; fi
done
