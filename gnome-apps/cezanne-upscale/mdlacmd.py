"""MDLA 1.5 command building for the int16 networks (the part of
mainline/apu/re/mdla.py the upscaler needs; field meanings in
mainline/apu/README.md): 0x1c0-byte register images, the conv geometry,
int16 mode, the int16 weight packing."""
import struct

import numpy as np


class Cmd:
    def __init__(self, words):
        self.w = list(words)

    def __getitem__(self, off):
        return self.w[off // 4]

    def __setitem__(self, off, val):
        self.w[off // 4] = val & 0xffffffff

    def bytes(self):
        return struct.pack('<112I', *self.w)


# the words of a conv command that no builder here sets (engine / CBUF modes,
# found with a compiled 3x3 conv as the first template, mainline/apu/README.md)
TEMPLATE = {
    0x070: 0xf0011000, 0x090: 0x00100010, 0x0ac: 0x00000001, 0x0bc: 0x20f00081,
    0x0c8: 0x0000040c, 0x118: 0x0000f001, 0x148: 0x00003080, 0x15c: 0x200186aa,
}


def template():
    c = Cmd([0] * 112)
    for off, v in TEMPLATE.items():
        c[off] = v
    return c


def wblock(K, cpitch, esz=1):
    """weight block size (0x04c): half a 16-output-channel tile, rounded up to
    256 bytes (esz 2: int16 weights)"""
    return -(-K * K * cpitch * 8 * esz // 256) * 256


def pack_conv_weights16(wt, blk=None):
    """int16 weights [co][ky][kx][ci] -> MDLA order: slot (2 bytes) = cot * 2
    blk + (ci & 1) * blk + (ci >> 1) * K*K*16 + tap * 16 + co. Odd CI padded
    with a zero channel, blocks to blk bytes."""
    co, kh, kw, ci = wt.shape
    cp = -(-ci // 2) * 2
    if cp != ci:
        wt = np.concatenate([wt, np.zeros((co, kh, kw, cp - ci), wt.dtype)], axis=3)
    a = wt.astype(np.int16).reshape(co // 16, 16, kh * kw, cp // 2, 2)
    a = np.ascontiguousarray(a.transpose(0, 4, 3, 2, 1)).reshape(co // 16, 2, -1)
    n = (blk or 2 * a.shape[2]) // 2
    out = np.zeros((co // 16, 2, n), '<i2')
    out[:, :, :a.shape[2]] = a
    return out.tobytes()


def input_pingpong(c, bank):
    """with spatial tiles the MDLA loads the next tile's input while it works
    on one: the second input buffer at CBUF bank 0x120 [23:16] (input
    addresses wrap at bank 0x60, so each holds half of it; without it the
    load overwrites the tile in use, re/e21_tiling.py)"""
    c[0x120] = bank << 16


def int16_mode(c):
    """int16 activations and weights: output type 0x05c [14:12] = 6, 0x0c8
    [1:0] = 3, 0x0cc [7:6] = 3, 0x0fc = 1, 0x15c [11:0] = 0x555"""
    c[0x05c] = (c[0x05c] & ~(7 << 12)) | 6 << 12
    c[0x0c8] = c[0x0c8] | 3
    c[0x0cc] = (c[0x0cc] & 0x3f) | 0xc0
    c[0x0fc] = 1
    c[0x15c] = (c[0x15c] & ~0xfff) | 0x555


def conv_geometry(c, H, W, CI, CO, K=3, pad=(1, 1, 1, 1), cpitch=None, opitch=None, esz=2,
                  th=1, tw=1, cout_tiles=True):
    """geometry of a conv command over an H x W input window, th x tw
    spatial tiles the MDLA walks itself (To x Tw output px each, the last
    ones E px short; needs the second input buffer, input_pingpong()),
    16-channel output tiles (cout_tiles=False: all CO at once, the weights
    of the whole layer in the CBUF, as MediaTek's compiled nets tile; else
    each spatial tile's input is loaded once per output tile, re/e21);
    pad = (top, bottom, left, right); channel pitches in elements, sizes in
    bytes"""
    t, b, l, r = pad
    cp, op = cpitch or CI, opitch or CO
    Ho, Wo = H + t + b - K + 1, W + l + r - K + 1
    To, Tw = -(-Ho // th), -(-Wo // tw)
    Eh, Ew = th * To - Ho, tw * Tw - Wo
    # an empty last tile hangs the MDLA until a reboot
    assert Eh < To and Ew < Tw, f'{th} x {tw} tiles leave an empty one ({Ho} x {Wo} out)'
    g = 2 if esz == 2 else 4			# MAC channel group
    c[0x00c] = H << 16 | 1
    c[0x010] = W
    c[0x014] = CI * 16
    c[0x018] = K << 16 | CO
    c[0x01c] = CI << 16 | K
    c[0x020] = Ho << 16 | 1
    c[0x024] = CO << 16 | Wo
    c[0x028] = Ho * Wo * op
    c[0x02c] = Wo * op
    c[0x030] = op
    c[0x034] = H * W * cp
    c[0x038] = W * cp
    c[0x03c] = cp
    c[0x04c] = wblock(K, -(-CI // g) * g, esz)
    ct = CO // 16 if cout_tiles else 1
    c[0x074] = 1 << 24 | ct << 16 | th << 8 | tw
    c[0x078] = ct * th * tw
    if not cout_tiles:
        c[0x090] = 1 << 16 | CO
        c[0x0c8] &= ~0x400
    c[0x07c] = (To - 1 + K) << 16 | (Tw - 1 + K)
    c[0x080] = To << 16 | Tw
    c[0x084] = (Eh + b) << 16 | t
    c[0x088] = (Ew + r) << 16 | l
    c[0x08c] = c[0x09c] = To << 16 | Tw
    c[0x094] = c[0x0a0] = Eh << 16
    c[0x098] = c[0x0a4] = Ew << 16
    c[0x0c0] = 0x11
    c[0x0c4] = (c[0x0c4] & 0xff0000) | b << 12 | t << 8 | r << 4 | l
    c[0x19c] = H * W * cp * esz
    c[0x1a4] = Ho * Wo * op * esz
    return Ho, Wo
