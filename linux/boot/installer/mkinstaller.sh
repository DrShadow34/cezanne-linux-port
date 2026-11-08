#!/usr/bin/bash
# Build the installer ramdisk tree from the running Arch system (run it on
# the phone): mkinstaller.sh <dir>. Programs + their libraries, host keys and
# root's authorized_keys; then on the PC: mkboot-mainline.sh <dir> out.img
set -euo pipefail
D=${1:?target dir}
HERE=$(cd "$(dirname "$0")" && pwd)
rm -rf "$D"; mkdir -p "$D"/{usr/bin,usr/lib,etc/ssh,root/.ssh,dev,proc,sys,run,tmp,var/empty,usr/share/empty.sshd,mnt}
for l in bin sbin usr/sbin; do ln -s usr/bin "$D/$l"; done
ln -s usr/lib "$D/lib"; ln -s usr/lib "$D/lib64"
BINS=(bash cat ls cp mv rm mkdir rmdir ln chmod chown sleep sync dd sha256sum md5sum head tail
      stat df du sort uniq tr cut wc date echo env true false uname readlink realpath mktemp tee
      seq basename dirname printf test touch truncate nproc od cmp
      mount umount blkdiscard blockdev lsblk findmnt fstrim losetup dmesg kill blkid wipefs sfdisk
      ps free pkill pgrep grep sed gawk awk find xargs
      mke2fs mkfs.ext4 e2fsck fsck.ext4 resize2fs tune2fs dumpe2fs e2label debugfs e2image
      mkfs.btrfs btrfs btrfstune btrfs-convert tar getfattr getfacl
      zstd gzip ip dnsmasq sshd)
LIBEXEC=(/usr/lib/ssh/sshd-session /usr/lib/ssh/sshd-auth /usr/lib/ssh/sftp-server)
copy_libs() {
	ldd "$1" 2>/dev/null | grep -o '/[^ )]*' | while read -r lib; do
		[ -e "$D$lib" ] || cp -L "$lib" "$D$lib"
	done
}
for b in "${BINS[@]}"; do
	p=$(type -P "$b")
	cp -L "$p" "$D/usr/bin/$b"
	copy_libs "$p"
done
for p in "${LIBEXEC[@]}"; do
	mkdir -p "$D$(dirname "$p")"
	cp -L "$p" "$D$p"
	copy_libs "$p"
done
ln -sf bash "$D/usr/bin/sh"
install -m755 "$HERE/init" "$D/init"
install -m755 "$HERE/reboot" "$D/usr/bin/reboot"
cp -a /etc/ssh/ssh_host_* "$D/etc/ssh/"
install -m600 /root/.ssh/authorized_keys "$D/root/.ssh/authorized_keys"
chmod 700 "$D/root" "$D/root/.ssh"
cat > "$D/etc/ssh/sshd_config" <<EOC
HostKey /etc/ssh/ssh_host_ed25519_key
HostKey /etc/ssh/ssh_host_ecdsa_key
HostKey /etc/ssh/ssh_host_rsa_key
PermitRootLogin prohibit-password
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
Subsystem sftp /usr/lib/ssh/sftp-server
EOC
printf 'root:x:0:0:root:/root:/usr/bin/bash\nsshd:x:22:22::/var/empty:/usr/bin/false\nnobody:x:65534:65534::/:/usr/bin/false\n' > "$D/etc/passwd"
printf 'root:x:0:\nsshd:x:22:\nnobody:x:65534:\n' > "$D/etc/group"
printf 'root:*:19000::::::\nsshd:!*:19000::::::\nnobody:!*:19000::::::\n' > "$D/etc/shadow"
chmod 600 "$D/etc/shadow"
echo 'passwd: files' > "$D/etc/nsswitch.conf"; echo 'group: files' >> "$D/etc/nsswitch.conf"; echo 'shadow: files' >> "$D/etc/nsswitch.conf"
echo cezanne-installer > "$D/etc/hostname"
du -sh "$D"
