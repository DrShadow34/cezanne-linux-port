"""Real-ESRGAN x4plus in int16 on the MDLA cores (/dev/cezanne-apu, driver
mainline/apu/cezanne-apu.c), with the parameters from export16.py:

    net = X4Plus(load_model('x4plus-int16.npz'), tile=pick_tile(H, W))
    big = net.upscale(rgb, progress=lambda done, total: ...)   # 4x, uint8
    net.close()

A picture runs as tiles of `tile` px plus PAD px of context on each side
(edge pixels repeated at the borders, as realesrgan-ncnn-vulkan does).
Each core runs a whole tile of its own: its own buffers and command stream
(the weights are shared), built once, all of a tile's layers in one
submission. Consecutive commands are ordered by the 0x15c bits as in
MediaTek's compiled networks (bit 15: a later command waits for this one,
bit 16: this one waits for the one before; first command 15, last 16).
Each layer region is one command whose output the MDLA walks in spatial
tiles itself (tiling(), mainline/apu/README.md "Spatial tiling"): the next
tile's input loads into a second CBUF buffer while one is computed, so a
tile holds at most half the 0x60 input banks; the shapes are picked for
the least padded work (output columns pad to 8, rows to 4). Layers whose
weights fit the CBUF run with all output channels at once (else the input
tile is reloaded per 16 channels: 1x1 adds 266 -> 480 MAC / cycle).

Buffers per core (int16, channel pitch in brackets):
  IN [4] RGBX;  X [384] = x0 | c5 | x | o1 o2 o3 o4 | feat, the whole
  trunk: c1..c4 read x | o1.. and write the next o, c5 reads those 192 and
  writes c5, the inner adds read c5 | x, the RRDB's add x0 | c5 | x, both
  write x in place (1x1 with all output channels at once: each spatial
  tile's input is in the CBUF before its output goes out), an identity
  copy x -> x0 starts the next RRDB; body writes o3 o4, so body | feat is
  the body add's input.
The tail computes only what the tile's own output needs (the 4x region
without the 4 * PAD margins, plus the convolutions' halos), in horizontal
stripes of STRIPE output rows from the body add on, so only X is whole:
per stripe the body add -> U1 [64] at 2N, up1 -> V1, U2 = nearest 2x of
V1, up2 -> V2, hr -> U2 again, last -> LOUT [16] (1.0 = 32640), which
shares X's memory (the stripes' output stays below the X rows still to be
read). Upsampling: the body add and an identity copy of up1's output write
their results four times with pixel and row pitch doubled (nearest 2x).
"""
import fcntl
import functools
import json
import mmap
import os
import struct

import numpy as np

import mdlacmd as mc

DEV = '/dev/cezanne-apu'
IOC_ALLOC = 0xc0104100		# _IOWR('A', 0, struct cz_apu_alloc)
IOC_RUN = 0xc0184101		# _IOWR('A', 1, struct cz_apu_run)
PAD = 10
STRIPE = 96			# rows of the 4N stages at a time
CBUF_BANK = 4096
CBUF_INPUT = 0x60		# input banks: 96 is the most the MDLA uses
CBUF_TOP = 0x7e			# banks 0x7e / 0x7f are not ours (re/e17_cbuf.py)
PATCH_MAX = 62			# windows up to 64 px are tested
WAIT_PREV, SIGNAL_NEXT = 1 << 16, 1 << 15	# command word 0x15c


def ceil8(v):
    return -(-v // 8) * 8


def ceil4(v):
    return -(-v // 4) * 4


def ceil2(v):
    return -(-v // 2) * 2


PLAN = {}			# experiments: (CI, CO, K) -> (patch, 0x124)


TILE_COST = 8e4			# a spatial tile's cost in MACs (~0.2 us, re/e21_tiling.py)
CBUF_HALF = CBUF_INPUT // 2		# tiled: input tile buffers [0, 0x30) and [0x30, 0x60)


@functools.lru_cache(maxsize=None)
def tiling(R, C, CI, CO, K):
    """spatial tiles (th, tw) for one command over an R x C output region:
    each tile's input window (To + K - 1 rows, Tw + K - 1 columns, both
    rounded up to 8) must fit one input buffer; the least padded work
    (output columns rounded up to 8, rows to 4: re/e21_tiling.py, 506 MAC /
    cycle at full tiles) plus a small cost per tile. The last tile in each
    direction may be shorter, never empty (that hangs the MDLA)."""
    room = CBUF_HALF * CBUF_BANK // (ceil2(CI) * 2)
    macs_px = CO * CI * K * K
    best = None
    for th in range(1, min(R, 255) + 1):
        To = -(-R // th)
        if th * To - R >= To or ceil8(To + K - 1) * 8 > room:
            continue
        rows = (th - 1) * ceil4(To) + ceil4(R - (th - 1) * To)
        for tw in range(1, min(C, 255) + 1):
            if best and th * tw * TILE_COST + R * C * macs_px >= best[0]:
                break			# no padding at all would not beat it
            Tw = -(-C // tw)
            if tw * Tw - C >= Tw or ceil8(To + K - 1) * ceil8(Tw + K - 1) > room:
                continue
            cols = (tw - 1) * ceil8(Tw) + ceil8(C - (tw - 1) * Tw)
            cost = th * tw * TILE_COST + rows * cols * macs_px
            if best is None or cost < best[0]:
                best = (cost, th, tw)
    return best[1], best[2]


def cbuf_plan(CI, CO, K):
    """(largest output patch (rows, cols), 0x124 word) for an int16 conv
    (re/e17_cbuf.py, mainline/apu/README.md "CBUF"): banks [0, lo) hold the
    input window plus the conv's padding, each side rounded up to 8 px, at
    most 0x60 banks (more lo does not add room); [lo, hi) the weights of one
    16-channel output tile, [hi, hi + tile) the next one's (double buffer),
    below bank 0x7e"""
    if (CI, CO, K) in PLAN:
        return PLAN[(CI, CO, K)]
    lo = CBUF_INPUT
    tile = 2 * mc.wblock(K, -(-CI // 2) * 2, 2)
    hi = lo + -(-tile // CBUF_BANK)
    assert CO <= 16 or hi * CBUF_BANK + tile <= CBUF_TOP * CBUF_BANK
    room = lo * CBUF_BANK // ((-(-CI // 2) * 2) * 2)	# window pixels (rounded) that fit
    fits = [(py, px) for py in range(4, PATCH_MAX + 1) for px in range(4, PATCH_MAX + 1)
            if ceil8(py + K - 1) * ceil8(px + K - 1) <= room]
    return fits, hi << 16 | lo


class Apu:
    """/dev/cezanne-apu: powered while open, one DMA buffer"""
    def __init__(self, size):
        self.fd = os.open(DEV, os.O_RDWR | os.O_CLOEXEC)
        try:
            r = fcntl.ioctl(self.fd, IOC_ALLOC, struct.pack('<QQ', size, 0))
            self.base = struct.unpack('<QQ', r)[1]
            self.mm = mmap.mmap(self.fd, size)
        except BaseException:
            os.close(self.fd)
            raise
        self.size = size

    def put(self, off, data):
        self.mm[off:off + len(data)] = data

    def get(self, off, n):
        return self.mm[off:off + n]

    def run(self, parts, timeout_ms=2000):
        """parts: (command offset, count) per core, count 0: core idle"""
        off, n = [0, 0], [0, 0]
        for i, (o, c) in enumerate(parts):
            off[i], n[i] = o, c
        fcntl.ioctl(self.fd, IOC_RUN, struct.pack('<6I', off[0], off[1], n[0], n[1], timeout_ms, 0))

    def close(self):
        self.mm.close()
        os.close(self.fd)


def load_model(path):
    z = np.load(path)
    layers = json.loads(bytes(z['meta']).decode())
    for i, l in enumerate(layers):
        l['w'] = z[f'w{i}'].tobytes()
        l['bias'] = z[f'b{i}'].tobytes()
    return {l['name']: l for l in layers}


def pick_tile(H, W, cores=2, tmax=600, tmin=64):
    """the tile size with the least work for an H x W picture: a tile costs
    (T + 2 PAD)^2, the cores take one tile each at a time. DMA buffer for
    two cores: 600 ~800 MB (3472 x 4620 -> 579: 6 x 8 tiles), 400 ~400 MB
    (-> 386), 200 ~160 MB (-> 193)"""
    best = None
    for T in range(tmin, tmax + 1):
        n = -(-H // T) * -(-W // T)
        cost = -(-n // cores) * (T + 2 * PAD) ** 2
        if best is None or cost < best[0]:
            best = (cost, T)
    return best[1]


def grow(r, k, lim):
    return max(r[0] - k, 0), min(r[1] + k, lim)


def half(r, lim):
    """the half-resolution range whose nearest 2x upsampling covers r"""
    return r[0] // 2, min(-(-r[1] // 2), lim)


class Region:
    """a buffer holding rows y0.. / columns x0.. of a frame of Hf x Wf"""
    def __init__(self, off, frame, rows, cols, pitch):
        self.off, self.pitch = off, pitch
        self.Hf, self.Wf = frame
        self.y0, self.x0 = rows[0], cols[0]
        self.H, self.W = rows[1] - rows[0], cols[1] - cols[0]

    def size(self):
        return self.H * self.W * self.pitch * 2

    def at(self, y, x, c):
        assert self.y0 <= y < self.y0 + self.H and self.x0 <= x < self.x0 + self.W
        return self.off + (((y - self.y0) * self.W + x - self.x0) * self.pitch + c) * 2


class X4Plus:
    def __init__(self, model, tile=192, cores=2, deps='all'):
        """deps: 'all' (every command ordered, as the compiled nets),
        'layers' (only layer boundaries), 'none' (one submission per layer)"""
        self.tile, self.N, self.cores, self.deps = tile, tile + 2 * PAD, cores, deps
        self.top = 0
        P = dict(model)
        for l in P.values():
            l['w_off'] = self.alloc(len(l['w']), 256)
            l['b_off'] = self.alloc(len(l['bias']), 64)
            if l['prelu']:
                l['t_off'] = self.alloc(0x800, 256)
        ident = dict(name='ident', K=1, CI=64, CO=64, mult=0x40000000, sh=0, prelu=None)
        w = np.zeros((64, 1, 1, 64), np.int16)
        w[np.arange(64), 0, 0, np.arange(64)] = 2		# * 0.5: an exact copy
        ident['w'] = mc.pack_conv_weights16(w, mc.wblock(1, 64, 2))
        ident['bias'] = bytes(256)
        ident['w_off'] = self.alloc(len(ident['w']), 256)
        ident['b_off'] = self.alloc(256, 64)
        self.P, self.ident = P, ident

        self.streams = [self.build() for _ in range(cores)]
        self.names = [n for n, _ in self.streams[0]['layers']]
        self.size = -(-self.top // 4096) * 4096
        self.apu = Apu(self.size)
        for s in self.streams:
            s['layers'] = self.place(s['layers'], s['cmd_off'])
        for l in list(P.values()) + [ident]:
            self.apu.put(l['w_off'], l['w'])
            self.apu.put(l['b_off'], l['bias'])
            if l['prelu']:
                pm, ps, nm, ns = l['prelu']
                self.apu.put(l['t_off'], np.full(l['CO'], nm, '<i4').tobytes())
                self.apu.put(l['t_off'] + 0x400, np.full(l['CO'], ns, np.uint8).tobytes())
        self.ncmd = sum(n for _, n in self.streams[0]['layers'])

    def alloc(self, n, align=4096):
        self.top = -(-self.top // align) * align
        off = self.top
        self.top += n
        return off

    def build(self):
        """one core's buffers and command stream (addresses relative to the
        DMA buffer until place())"""
        N, T, P = self.N, self.tile, self.P
        n1 = (0, N)
        # RGBX rows padded to 16 bytes (the MDLA needs 16-byte row pitches:
        # an odd N gave streaks of garbage)
        Wi = -(-N // 2) * 2
        IN = Region(self.alloc(N * Wi * 4 * 2), (N, N), n1, (0, Wi), 4)
        X = Region(self.alloc(N * N * 384 * 2), (N, N), n1, n1, 384)
        layers = []

        def conv(name, l, src, sc0, CI, dst, dc0, rows, cols, up=None):
            """output rows / columns of the src frame (up: written to the 2x
            frame of dst at 2y + dy, 2x + dx)"""
            K, CO = l['K'], l['CO']
            y0, x0 = rows[0], cols[0]
            ph, pw = rows[1] - y0, cols[1] - x0
            th, tw = tiling(ph, pw, CI, CO, K)
            # all output channels at once when the layer's weights fit
            wtile = 2 * mc.wblock(K, ceil2(CI), 2)		# one 16-channel output tile
            one = CBUF_INPUT * CBUF_BANK + CO // 16 * wtile <= CBUF_TOP * CBUF_BANK
            if K == 3:
                t, b = int(y0 == 0), int(y0 + ph == src.Hf)
                lf, r = int(x0 == 0), int(x0 + pw == src.Wf)
                iy, ix = y0 - 1 + t, x0 - 1 + lf
                ih, iw = ph + 2 - t - b, pw + 2 - lf - r
            else:
                # in place (src = dst) only like this: a tile's input is
                # read once, before its output is written
                assert one or src is not dst, name
                t = b = lf = r = 0
                iy, ix, ih, iw = y0, x0, ph, pw
            assert src.y0 <= iy and iy + ih <= src.y0 + src.H
            assert src.x0 <= ix and ix + iw <= src.x0 + src.W
            c = mc.template()
            dp = dst.pitch
            assert src.W * src.pitch * 2 % 16 == 0 and dst.W * dp * 2 % 16 == 0, name
            mc.conv_geometry(c, ih, iw, CI, CO, K=K, pad=(t, b, lf, r),
                             cpitch=src.pitch, opitch=2 * dp if up else dp,
                             th=th, tw=tw, cout_tiles=not one)
            mc.int16_mode(c)
            if th * tw > 1:
                mc.input_pingpong(c, CBUF_HALF)
            c[0x038] = src.W * src.pitch
            c[0x000] = src.at(iy, ix, sc0)
            if up:
                c[0x02c] = 2 * dst.W * dp
                c[0x008] = dst.at(2 * y0 + up[0], 2 * x0 + up[1], dc0)
                dst.at(2 * (y0 + ph - 1) + up[0], 2 * (x0 + pw - 1) + up[1], dc0)
            else:
                c[0x02c] = dst.W * dp
                c[0x008] = dst.at(y0, x0, dc0)
                dst.at(y0 + ph - 1, x0 + pw - 1, dc0)
            c[0x004] = l['w_off']
            c[0x054] = l['b_off']
            c[0x058] = l['mult']
            c[0x05c] = 6 << 12 | l['sh'] << 8
            c[0x124] = CBUF_INPUT << 16 | CBUF_INPUT if one else cbuf_plan(CI, CO, K)[1]
            c[0x15c] &= ~(WAIT_PREV | SIGNAL_NEXT)
            if l['prelu']:
                pm, ps, nm, ns = l['prelu']
                c[0x070] |= 0x300000
                c[0x0cc] = 0xf0
                c[0x0d0] = 0x0f000000
                c[0x0e0] = 7
                c[0x0e8] = ps << 17
                c[0x0f0] = pm
                c[0x14c] = 0x18100000
                c[0x16c] = l['t_off']
                c[0x170] = l['t_off'] + 0x400
            layers.append((name, [c]))

        # the trunk, whole frames
        conv('first', P['first'], IN, 0, 3, X, 128, n1, n1)
        conv('first>x0', P['first'], IN, 0, 3, X, 0, n1, n1)
        conv('first>feat', P['first'], IN, 0, 3, X, 320, n1, n1)
        for r in range(23):
            for d in range(3):
                for k in range(4):
                    conv(f'r{r}.{d}.c{k + 1}', P[f'r{r}.{d}.c{k + 1}'], X, 128, 64 + 32 * k,
                         X, 192 + 32 * k, n1, n1)
                conv(f'r{r}.{d}.c5', P[f'r{r}.{d}.c5'], X, 128, 192, X, 64, n1, n1)
                if d < 2:
                    conv(f'r{r}.{d}.add', P[f'r{r}.{d}.add'], X, 64, 128, X, 128, n1, n1)
                else:
                    conv(f'r{r}.add', P[f'r{r}.add'], X, 0, 192, X, 128, n1, n1)
                    if r < 22:
                        conv(f'r{r}.x0', self.ident, X, 128, 64, X, 0, n1, n1)

        # the tail: the regions the tile's output needs, from the end back
        R4 = (4 * PAD, 4 * PAD + 4 * T)
        U2r = grow(R4, 3, 4 * N)
        V1r = half(U2r, 2 * N)
        U1r = grow(V1r, 1, 2 * N)
        Fr = half(U1r, N)
        conv('body', P['body'], X, 128, 64, X, 256, Fr, Fr)
        # from the body add on in stripes of output rows
        cs = (U2r[0] // 2 * 2, min(-(-U2r[1] // 2) * 2, 4 * N))	# even: whole 2x2 copies
        U1c = (2 * Fr[0], 2 * Fr[1])
        rows_max = STRIPE + 8
        rows2_max = rows_max // 2 + 4
        S0 = self.alloc(rows2_max * (U1c[1] - U1c[0]) * 64 * 2)
        SV = self.alloc(rows2_max * (V1r[1] - V1r[0]) * 64 * 2)
        S1 = self.alloc(rows_max * (cs[1] - cs[0]) * 64 * 2)
        S2 = self.alloc(rows_max * (cs[1] - cs[0]) * 64 * 2)
        # LOUT shares X's memory: what the stripes so far wrote stays below
        # the X rows the later stripes' body adds read (asserted below)
        LOUT = Region(X.off, (4 * N, 4 * N), R4, R4, 16)
        assert LOUT.size() <= X.size()
        lout_end = X.off
        for r0 in range(R4[0], R4[1], STRIPE):
            r = (r0, min(r0 + STRIPE, R4[1]))
            ur = grow(r, 3, 4 * N)
            ur = (ur[0] // 2 * 2, min(-(-ur[1] // 2) * 2, 4 * N))
            vr = (ur[0] // 2, ur[1] // 2)
            fr = half(grow(vr, 1, 2 * N), N)
            U1 = Region(S0, (2 * N, 2 * N), (2 * fr[0], 2 * fr[1]), U1c, 64)
            V1 = Region(SV, (2 * N, 2 * N), vr, V1r, 64)
            U2 = Region(S1, (4 * N, 4 * N), ur, cs, 64)
            V2 = Region(S2, (4 * N, 4 * N), grow(r, 2, 4 * N), cs, 64)
            assert U1.H <= rows2_max and V1.H <= rows2_max
            assert U2.H <= rows_max and V2.H <= rows_max
            assert lout_end <= X.at(fr[0], Fr[0], 256)
            for dy in (0, 1):
                for dx in (0, 1):
                    conv(f'body.add{dy}{dx}', P['body.add'], X, 256, 128, U1, 0, fr, Fr, (dy, dx))
            conv('up1', P['up1'], U1, 0, 64, V1, 0, vr, V1r)
            for dy in (0, 1):
                for dx in (0, 1):
                    conv(f'up1.up{dy}{dx}', self.ident, V1, 0, 64, U2, 0,
                         vr, (cs[0] // 2, cs[1] // 2), (dy, dx))
            conv('up2', P['up2'], U2, 0, 64, V2, 0, grow(r, 2, 4 * N), grow(R4, 2, 4 * N))
            conv('hr', P['hr'], V2, 0, 64, U2, 0, grow(r, 1, 4 * N), grow(R4, 1, 4 * N))
            conv('last', P['last'], U2, 0, 64, LOUT, 0, r, R4)
            lout_end = LOUT.at(r[1] - 1, R4[1] - 1, 0) + 16 * 2
        ncmd = sum(len(cs) for _, cs in layers)
        return dict(IN=IN, LOUT=LOUT, layers=layers, cmd_off=self.alloc(ncmd * 0x1c0, 4096))

    def place(self, layers, off):
        """dependency bits, ids, bus addresses; the commands into the DMA
        buffer at off -> [(command offset, count)] per submission"""
        cmds = [c for _, cs in layers for c in cs]
        bounds, a = [], 0
        for _, cs in layers:
            bounds.append((a, a + len(cs)))
            a += len(cs)
        base = self.apu.base
        for c in cmds:
            for o in (0x000, 0x004, 0x008, 0x054):
                c[o] = base + c[o]
            if c[0x16c]:
                c[0x16c] = base + c[0x16c]
                c[0x170] = base + c[0x170]
        if self.deps == 'none':
            groups = bounds
        else:
            groups = [(0, len(cmds))]
            for a, b in bounds:
                for i in range(a, b):
                    if self.deps == 'all' or i == a:
                        cmds[i][0x15c] |= WAIT_PREV
                    if self.deps == 'all' or i == b - 1:
                        cmds[i][0x15c] |= SIGNAL_NEXT
            cmds[0][0x15c] &= ~WAIT_PREV
            cmds[-1][0x15c] &= ~SIGNAL_NEXT
        out = []
        for a, b in groups:
            for i in range(a, b):
                cmds[i][0x150] = i - a + 1
                cmds[i][0x154] = (cmds[i][0x154] & ~(1 << 24)) | (1 << 24 if i == b - 1 else 0)
            out.append((off + a * 0x1c0, b - a))
        self.apu.put(off, b''.join(c.bytes() for c in cmds))
        return out

    def run_tiles(self, x8s):
        """up to `cores` tiles at once, one per core: uint8 N x N x 3 RGB each
        (the tile with its context) -> uint8 4 tile x 4 tile x 3 each"""
        N, T = self.N, self.tile
        for x8, s in zip(x8s, self.streams):
            xin = np.zeros((N, s['IN'].W, 4), '<i2')
            xin[:, :N, :3] = x8
            self.apu.put(s['IN'].off, xin.tobytes())
        used = self.streams[:len(x8s)]
        for k in range(len(used[0]['layers'])):
            self.apu.run([s['layers'][k] for s in used], 60000)
        out = []
        for s in used:
            L = s['LOUT']
            y = np.frombuffer(self.apu.get(L.off, L.size()), '<i2').reshape(L.H, L.W, 16)[..., :3]
            out.append(np.clip((y.astype(np.int32) + 64) >> 7, 0, 255).astype(np.uint8))
        return out

    def run_tile(self, x8):
        return self.run_tiles([x8])[0]

    def upscale(self, img, out=None, progress=None):
        """img uint8 H x W x 3 RGB -> 4H x 4W x 3 (into out if given)"""
        H, W = img.shape[:2]
        T = self.tile
        ny, nx = -(-H // T), -(-W // T)
        pad = np.pad(img, ((PAD, PAD + ny * T - H), (PAD, PAD + nx * T - W), (0, 0)), mode='edge')
        if out is None:
            out = np.empty((4 * H, 4 * W, 3), np.uint8)
        tiles = [(ty, tx) for ty in range(ny) for tx in range(nx)]
        for i in range(0, len(tiles), self.cores):
            group = tiles[i:i + self.cores]
            ys = self.run_tiles([pad[ty * T:ty * T + self.N, tx * T:tx * T + self.N] for ty, tx in group])
            for (ty, tx), y in zip(group, ys):
                h, w = min(4 * T, 4 * (H - ty * T)), min(4 * T, 4 * (W - tx * T))
                out[4 * T * ty:4 * T * ty + h, 4 * T * tx:4 * T * tx + w] = y[:h, :w]
            if progress:
                progress(i + len(group), len(tiles))
        return out

    def close(self):
        self.apu.close()
