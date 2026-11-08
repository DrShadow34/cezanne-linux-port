#!/usr/bin/env python3
# extract-dsp.py LIBMNL OUTDIR: take the CONNSYS GNSS DSP RAM code (L1 and
# L5, MT6885 / CONNAC 2.0 "CONNACL1/L5_DSP_0041_0315") out of MIUI's
# /vendor/lib64/libmnl.so (V14.0.5.0.SJNCNXM) as gnss-l1.bin / gnss-l5.bin.
# These are firmware blobs, stored encrypted (the DSP ROM decrypts them,
# header key 0x7B); gnss-boot.py downloads them. Offsets and digests are
# for that libmnl build; another build needs new offsets.
import hashlib
import os
import sys

IMAGES = {
    'gnss-l1.bin': (0x166bb6, 69758, 'L1'),
    'gnss-l5.bin': (0x36a0e4, 36624, 'L5'),
}
DIGEST = {
    'gnss-l1.bin': '134cfb93d1688852e9d39ab1df648d785268ab6e695956e1a8c72661fab432d7',
    'gnss-l5.bin': '5bae046203077f557593f94bc2e01bcaa56bd8b80e0d8e0a9d5aab187c53e992',
}

lib = open(sys.argv[1], 'rb').read()
os.makedirs(sys.argv[2], exist_ok=True)
for name, (off, size, link) in IMAGES.items():
    img = lib[off:off + size]
    digest = hashlib.sha256(img).hexdigest()
    if digest != DIGEST[name]:
        sys.exit(f'{name}: sha256 {digest} does not match; a different libmnl build?')
    with open(os.path.join(sys.argv[2], name), 'wb') as f:
        f.write(img)
    print(f'{name}: {link} DSP image, {size} bytes at {off:#x}')
