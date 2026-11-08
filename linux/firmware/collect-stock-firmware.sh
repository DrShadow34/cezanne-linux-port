#!/bin/bash
# collect-stock-firmware.sh [-s SUPER] [-v VENDOR_IMG] [-c SCP1] [OUT]
#
# Collects the firmware the cezanne drivers load from the phone's stock MIUI
# partitions (or images of them) into OUT, default stock-firmware.tar.zst
# next to this script: the source of the cezanne-firmware package (makepkg
# here afterwards). Nothing is written to the partitions.
#   super, logical partition "vendor":
#     /firmware      CONNSYS Wi-Fi / Bluetooth / FM blobs and configs,
#                    CS35L41 speaker DSP tunings, AW8697 haptics library
#     /etc/mdota     the modem's OTA configuration files
#     /lib64/libmnl.so  carries the GNSS DSP RAM images
#   scp1             the SCP sensor hub image
#
# Default sources are the phone's own partitions: run it as root on the
# phone while MIUI's super partition still holds the stock system (before
# it is reused). Images work as well: -s a dd of super or the fastboot
# ROM's super.img (sparse), -v an unpacked vendor image, -c a dd of scp1.
# Needs lpunpack and simg2img (android-tools), debugfs (e2fsprogs), zstd.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
super=/dev/disk/by-partlabel/super
scp=/dev/disk/by-partlabel/scp1
vendor=
while getopts s:v:c: o; do
	case $o in
	s) super=$OPTARG ;;
	v) vendor=$OPTARG ;;
	c) scp=$OPTARG ;;
	*) sed -n '2,3p' "$0"; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
out=$(realpath -m "${1:-$here/stock-firmware.tar.zst}")

tmp=$(mktemp -d "${TMPDIR:-/var/tmp}/cezanne-fw.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

if [[ -z $vendor ]]; then
	src=$super
	if [[ $(head -c 4 "$super" | od -An -tx4 | tr -d ' ') == ed26ff3a ]]; then
		echo "super: sparse image, expanding"
		simg2img "$super" "$tmp/super.raw"
		src=$tmp/super.raw
	fi
	lpunpack -p vendor "$src" "$tmp"
	rm -f "$tmp/super.raw"
	vendor=$tmp/vendor.img
fi

st=$tmp/stage
mkdir -p "$st/vendor/etc" "$st/vendor/lib64" "$tmp/fw"
debugfs -R "rdump /firmware $tmp/fw" "$vendor" 2>/dev/null
debugfs -R "rdump /etc/mdota $st/vendor/etc" "$vendor" 2>/dev/null
debugfs -R "dump /lib64/libmnl.so $st/vendor/lib64/libmnl.so" "$vendor" 2>/dev/null
mkdir -p "$st/vendor/firmware"
# only what the drivers load (the vendor's /firmware has 34 MB of other parts)
for f in "$tmp"/fw/firmware/{soc3_0_*,WIFI_RAM_CODE_*,mt6635_fm_*,*.cfg,aw8697_haptic.bin,cs35l41-*,RCV-cs35l41-*}; do
	[[ -f $f ]] && cp -p "$f" "$st/vendor/firmware/"
done
dd if="$scp" of="$st/scp1.img" bs=1M status=none

for f in vendor/firmware/WIFI_RAM_CODE_soc3_0_1_1.bin vendor/etc/mdota vendor/lib64/libmnl.so scp1.img; do
	[[ -e $st/$f ]] || { echo "missing $f: not the cezanne stock system?" >&2; exit 1; }
done
(cd "$st" && find . -type f | sort | xargs sha256sum) > "$tmp/SHA256SUMS"
mv "$tmp/SHA256SUMS" "$st/"
tar -C "$st" -cf - . | zstd -q -19 -T0 > "$out"
echo "$out: $(find "$st" -type f | wc -l) files"
