#!/usr/bin/env bash
# Build a recovery-slot test image for a mainline kernel:
#   shim + our DTB + Image  (lk always passes its own DTB; the shim swaps it)
# CHOOSER=1: boot-slot image that also carries MIUI's kernel + ramdisk (from
# the Magisk-patched stock boot.img); the shim boots MIUI when the one-shot
# RAM flag is set (cezanne-bootsel, `reboot miui`), else Arch.
# NO_MIUI=1 (with CHOOSER=1): boot-slot image without MIUI (MIUI wiped
# 2026-10-30); the shim then always boots Arch and counts no failed boots.
# Usage: mkboot-mainline.sh <ramdisk-dir> <out.img>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
M="$ROOT/mainline"
# KSRC / KOUT / W: kernel tree, its build dir, work dir (linux-cezanne's
# PKGBUILD points them at its own build)
KSRC="${KSRC:-$ROOT/src/linux-mainline}"
KOUT="${KOUT:-$ROOT/src/out-mainline}"
RD_DIR="$(realpath "${1:?ramdisk dir}")"
OUT="$(realpath -m "${2:?out image}")"
W="${W:-$ROOT/build/mainline}"
STOCK_BOOT="$ROOT/tools/magisk/magisk_patched_V14.0.5.0.img"
RAMDISK_ADDR=0x47c80000		# where lk loads the boot.img ramdisk

mkdir -p "$W"

# ramdisk first: its size goes into the DTB's /chosen
(cd "$RD_DIR" && find . | cpio -o -H newc -R 0:0 2>/dev/null | gzip -9) > "$W/ramdisk.gz"
rd_size=$(stat -c%s "$W/ramdisk.gz")
rd_end=$(printf '0x%x' $((RAMDISK_ADDR + rd_size)))

# DTB: preprocess with the mainline tree's dt-bindings headers, then dtc
# display: mediatek-drm + panel driver (default), or DISPLAY_DRV=simpledrm for
# lk's pipeline scanned out by simpledrm + the kick driver (fallback)
DT_DEFS=()
[[ "${DISPLAY_DRV:-drm}" == drm ]] && DT_DEFS+=(-DCEZANNE_DRM)
cpp -nostdinc -undef -x assembler-with-cpp -D__DTS__ "${DT_DEFS[@]}" \
    -I "$M/dts" -I "$KSRC/include" -I "$KSRC/scripts/dtc/include-prefixes" \
    "$M/dts/mt6885-xiaomi-cezanne.dts" > "$W/board.dts"
cat >> "$W/board.dts" <<EOF
&{/chosen} {
	linux,initrd-start = <0x0 $RAMDISK_ADDR>;
	linux,initrd-end = <0x0 $rd_end>;
};
EOF
# KEEP_WDT=1: let the kernel feed lk's watchdog (real boots); tests let it fire
[[ "${KEEP_WDT:-0}" == 1 ]] && sed -i -E "s/(\")?watchdog\.handle_boot_enabled=0 ?/\1/" "$W/board.dts"
# VERBOSE=1: full kernel log on screen and in pstore instead of the boot splash
[[ "${VERBOSE:-0}" == 1 ]] && sed -i "s/quiet splash loglevel=3 vt.global_cursor_default=0/loglevel=8 ignore_loglevel/" "$W/board.dts"
# BOOTARGS_EXTRA: appended to the kernel command line (debugging)
# CHOOSER=1: tell userspace it runs from the boot slot (dev-autoreboot off)
[[ "${CHOOSER:-0}" == 1 ]] && BOOTARGS_EXTRA="${BOOTARGS_EXTRA:+$BOOTARGS_EXTRA }cezanne.bootslot=boot"
[[ -n "${BOOTARGS_EXTRA:-}" ]] && sed -i "s|\(bootargs = \"[^\"]*\)\"|\1 ${BOOTARGS_EXTRA}\"|" "$W/board.dts"
"$KOUT/scripts/dtc/dtc" -q -I dts -O dtb -o "$W/board.dtb" "$W/board.dts"

# shim + DTB + Image, gzipped like a stock kernel
clang --target=aarch64-linux-gnu -nostdlib -ffreestanding -c "$M/shim/shim.S" -o "$W/shim.o"
clang --target=aarch64-linux-gnu -nostdlib -ffreestanding -fno-builtin -O2 \
    -mstrict-align -mgeneral-regs-only -fno-pic -fno-stack-protector -Wall \
    -c "$M/shim/shim.c" -o "$W/shim-c.o"
(cd "$W" && ld.lld -T "$M/shim/shim.lds" shim.o shim-c.o -o shim.elf)
llvm-objcopy -O binary "$W/shim.elf" "$W/shim.bin"
unpack_bootimg --boot_img "$STOCK_BOOT" --out "$W/stock" >/dev/null
MIUI_PARTS=()
if [[ "${CHOOSER:-0}" == 1 && "${NO_MIUI:-0}" != 1 ]]; then
    gzip -dc "$W/stock/kernel" > "$W/stock/Image"
    MIUI_PARTS=("$W/stock/Image" "$W/stock/ramdisk")
fi
python3 -I "$M/shim/mkshim.py" "$W/shim.bin" "$W/board.dtb" "$KOUT/arch/arm64/boot/Image" "$W/kernel.bin" "${MIUI_PARTS[@]}"
if [[ "${RAW_KERNEL:-0}" == 1 ]]; then cp "$W/kernel.bin" "$W/kernel.gz"; else gzip -n -9 -c "$W/kernel.bin" > "$W/kernel.gz"; fi	# lk chokes on the gzip FNAME field

# boot image v2 with the stock layout; lk ignores the dtb section, keep stock's.
# lk builds MIUI's command line from this one (stock: + buildvariant=user)
PART=recovery CMDLINE="bootopt=64S3,32N2,64N2"
[[ "${CHOOSER:-0}" == 1 ]] && PART=boot CMDLINE="bootopt=64S3,32N2,64N2 buildvariant=user"
PART=${AVB_PART:-$PART}	# AVB_PART=recovery: test a chooser image in the recovery slot
mkbootimg --header_version 2 --os_version 12.0.0 --os_patch_level 2023-07 \
    --kernel "$W/kernel.gz" --ramdisk "$W/ramdisk.gz" --dtb "$W/stock/dtb" \
    --pagesize 2048 --base 0 --kernel_offset 0x40080000 --ramdisk_offset 0x47c80000 \
    --tags_offset 0x4bc80000 --dtb_offset 0x4bc80000 \
    --cmdline "$CMDLINE" -o "$OUT"	# lk reads the kernel bitness from bootopt
# the footer must sit at the end of the real partition: lk's AVB checks boot
# AND recovery on every boot, and a footer pointing at no vbmeta (a 64 MiB
# image dd'ed over a 128 MiB recovery) makes lk assert on all boots.
PART_SIZE=67108864	# boot
[[ $PART == recovery ]] && PART_SIZE=134217728
avbtool add_hash_footer --image "$OUT" --partition_size $PART_SIZE \
    --partition_name $PART --algorithm NONE
echo "built $OUT (ramdisk $rd_size B, initrd end $rd_end)"
