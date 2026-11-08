#!/usr/bin/env python3
"""Pack shim.bin + DTB + arm64 Image into one blob lk can boot as "kernel".
With a MIUI Image and ramdisk it becomes the boot chooser (see shim.S).
Usage: mkshim.py shim.bin board.dtb Image out.img [miui-Image miui-ramdisk]"""
import struct
import sys

ALIGN = 4096

# Byte offsets of the patchable fields in shim.S
OFF_IMAGE_SIZE = 16
OFF_DESC = 64          # dtb, Image, MIUI Image, MIUI ramdisk: (offset, size) u64 pairs
# shim.c: C stack at 0x46f00000 sits right above the blob
MAX_TOTAL = 0x46e00000 - 0x40080000


def pad(data, align=ALIGN):
    return data + b"\0" * (-len(data) % align)


def main():
    shim_path, dtb_path, image_path, out_path = sys.argv[1:5]
    miui = open(sys.argv[5], "rb").read() if len(sys.argv) > 6 else b""
    ramdisk = open(sys.argv[6], "rb").read() if len(sys.argv) > 6 else b""
    shim = bytearray(pad(open(shim_path, "rb").read()))
    dtb = pad(open(dtb_path, "rb").read(), 8)
    image = open(image_path, "rb").read()

    if image[56:60] != b"ARM\x64":
        sys.exit(f"{image_path}: not an arm64 Image (missing ARM\\x64 magic)")
    if struct.unpack_from("<Q", image, 8)[0] != 0:
        sys.exit(f"{image_path}: unexpected non-zero text_offset")
    if len(dtb) > 0x80000:
        sys.exit("DTB larger than lk's 512 KiB dtb slot")

    dtb_off = len(shim)
    kern_off = dtb_off + len(pad(dtb))
    image = pad(image)
    if miui and miui[56:60] != b"ARM\x64":
        sys.exit(f"{sys.argv[5]}: not an arm64 Image")
    miui_off = kern_off + len(image)
    miui = pad(miui)
    rd_off = miui_off + len(miui)
    total = rd_off + len(pad(ramdisk, 8))
    if total > MAX_TOTAL:
        sys.exit(f"blob {total:#x} B overlaps the shim's stack")

    struct.pack_into("<Q", shim, OFF_IMAGE_SIZE, total)
    struct.pack_into("<8Q", shim, OFF_DESC, dtb_off, len(dtb), kern_off, len(image),
                     miui_off if miui else 0, len(miui),
                     rd_off if ramdisk else 0, len(ramdisk))

    with open(out_path, "wb") as f:
        f.write(shim)
        f.write(pad(dtb))
        f.write(image)
        f.write(miui)
        f.write(pad(ramdisk, 8))
    print(f"{out_path}: shim {len(shim)} B, dtb {len(dtb)} B @{dtb_off:#x}, "
          f"Image {len(image)} B @{kern_off:#x}, MIUI Image {len(miui)} B @{miui_off:#x}, "
          f"MIUI ramdisk {len(ramdisk)} B @{rd_off:#x}, total {total} B")


if __name__ == "__main__":
    main()
