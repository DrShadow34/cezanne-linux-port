# sourced by the m-*.sh steps (rescue system, 2026-11-02 ext4 -> btrfs)
dev=/dev/$(grep -l '^PARTNAME=userdata$' /sys/class/block/*/uevent | head -1 | cut -d/ -f5)
[ "$dev" = /dev/sdc60 ] || { echo "userdata is '$dev', expected /dev/sdc60"; exit 1; }
# not carried over: the build chroot (recreated as a subvolume), the package cache
PRUNE=(\( -path ./var/lib/cezanne/chroot -o -path './var/cache/pacman/pkg/*' -o -path ./lost+found \) -prune -o)
# manifest DIR OUT: metadata listing, sha256 of every file, all xattrs (caps, ACLs)
manifest() {
	mkdir -p "$2"
	cd "$1" || exit 1
	find . "${PRUNE[@]}" ! -type s -printf '%y %m %U %G %s %n %Ts %p\t%l\n' |
		gawk -F'\t' -v OFS='\t' '/^d / { $1 = gensub(/^(d [^ ]+ [^ ]+ [^ ]+) [^ ]+ [^ ]+ /, "\\1 ", 1, $1) } { print }' |
		LC_ALL=C sort > "$2/list"
	find . "${PRUNE[@]}" -type f -print0 | xargs -0 -P8 -n 500 sha256sum | LC_ALL=C sort -k2 > "$2/sums"
	find . "${PRUNE[@]}" -print0 | xargs -0 -n 500 getfattr -h -d -m - -e hex 2>/dev/null |
		awk '/^# file: / { f = $0; next } NF { print f "\t" $0 }' | LC_ALL=C sort > "$2/xattrs"
	wc -l "$2"/list "$2"/sums "$2"/xattrs
}
