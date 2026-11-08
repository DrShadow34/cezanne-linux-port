"""The compact Real-ESRGAN (compact.py's int16 model) on both MDLA cores:
frames (video) at 2x or 4x, in tiles of Th x Tw px plus PAD px of context
(edges repeated at the frame's borders), one tile per core at a time.

    net = Compact(load('compact-x2.npz'), tile=pick_tile(H, W))
    y = net.frame(rgb)           # uint8 H x W x 3 -> uint8 sH x sW x 3
    net.close()

Per core: A / B [64] (the 33 hidden layers ping-pong) and two sets of IN
[4] RGBX, OUT [16 or 48] and commands: 34 per tile, chained (0x15c) in one
submission, each a whole tile the MDLA tiles itself (x4plus_mdla.tiling),
the PReLU stage with per-channel negative factors (the table at 0x16c /
0x170). The pixel shuffle and the input's nearest upscale are added on the
CPU (compact.finish) while the MDLA works on the next tiles in the other
set (the device ioctl releases the GIL). Built once per tile size."""
import time

import numpy as np

import compact
import mdlacmd as mc
from x4plus_mdla import (Apu, CBUF_BANK, CBUF_HALF, CBUF_INPUT, CBUF_TOP, SIGNAL_NEXT, WAIT_PREV,
                         cbuf_plan, ceil2, tiling)

PAD = 16			# context per side (the network sees 34 px; Real-ESRGAN tiles use 10)


def load(path):
    z = np.load(path)
    return {k: z[k] for k in z.files}


MAX_TILE_PX = 300_000		# (Th + 2 PAD) (Tw + 2 PAD): ~100 MB of DMA buffer per core


def pick_tile(H, W, cores=2, tmin=64):
    """(Th, Tw) with the least work: ceil(tiles / cores) (Th + 2 PAD) (Tw + 2 PAD),
    even splits of the frame preferred by that cost"""
    best = None
    for ny in range(1, -(-H // tmin) + 1):
        Th = -(-H // ny)
        Th = -(-Th // 8) * 8
        for nx in range(1, -(-W // tmin) + 1):
            Tw = -(-W // nx)
            Tw = -(-Tw // 8) * 8
            if (Th + 2 * PAD) * (Tw + 2 * PAD) > MAX_TILE_PX:
                continue
            n = -(-H // Th) * -(-W // Tw)
            cost = -(-n // cores) * (Th + 2 * PAD) * (Tw + 2 * PAD)
            if best is None or cost < best[0]:
                best = (cost, (Th, Tw))
    return best[1]


class Compact:
    def __init__(self, model, tile=(256, 256), cores=2):
        if isinstance(tile, int):
            tile = (tile, tile)
        self.M, self.tile, self.cores = model, tile, cores
        self.scale = int(model['scale'])
        self.Nh, self.Nw = tile[0] + 2 * PAD, tile[1] + 2 * PAD
        self.co_last = 3 * self.scale * self.scale		# 12 / 48
        self.out_c = -(-self.co_last // 16) * 16
        self.top = 0
        self.layers = []
        for i in range(compact.N_CONV):
            wq = model[f'w{i}']
            co, _, _, ci = wq.shape
            cop = -(-co // 16) * 16
            if cop != co:
                wq = np.concatenate([wq, np.zeros((cop - co, 3, 3, ci), np.int16)])
            blk = mc.wblock(3, ceil2(ci), 2)
            L = dict(i=i, ci=ci, co=cop, blk=blk, w=mc.pack_conv_weights16(wq, blk))
            b = np.zeros(cop, np.int32)
            b[:co] = model[f'b{i}']
            L['b'] = b.tobytes()
            L['mult'], L['sh'] = (int(v) for v in model[f'rq{i}'])
            L['w_off'] = self.alloc(len(L['w']), 256)
            L['b_off'] = self.alloc(len(L['b']), 64)
            if f'pp{i}' in model:
                L['pm'], L['ps'] = (int(v) for v in model[f'pp{i}'])
                L['t_off'] = self.alloc(0x800, 256)
            self.layers.append(L)
        self.streams = [self.build() for _ in range(cores)]
        self.size = -(-self.top // 4096) * 4096
        self.apu = Apu(self.size)
        base = self.apu.base
        for L in self.layers:
            self.apu.put(L['w_off'], L['w'])
            self.apu.put(L['b_off'], L['b'])
            if 't_off' in L:
                i = L['i']
                self.apu.put(L['t_off'], model[f'pn{i}'].astype('<i4').tobytes())
                self.apu.put(L['t_off'] + 0x400, model[f'ps{i}'].astype(np.uint8).tobytes())
        for s in [st for core in self.streams for st in core]:
            cs = s['cmds']
            for k, c in enumerate(cs):
                for o in (0x000, 0x004, 0x008, 0x054):
                    c[o] = base + c[o]
                if c[0x16c]:
                    c[0x16c] = base + c[0x16c]
                    c[0x170] = base + c[0x170]
                c[0x15c] |= (WAIT_PREV if k else 0) | (SIGNAL_NEXT if k < len(cs) - 1 else 0)
                c[0x150] = k + 1
                c[0x154] = (c[0x154] & ~(1 << 24)) | (1 << 24 if k == len(cs) - 1 else 0)
            self.apu.put(s['cmd_off'], b''.join(c.bytes() for c in cs))
            s['run'] = (s['cmd_off'], len(cs))

    def alloc(self, n, align=4096):
        self.top = -(-self.top // align) * align
        off = self.top
        self.top += n
        return off

    def build(self):
        """one core: A / B shared, two sets of IN / OUT / commands"""
        Nh, Nw = self.Nh, self.Nw
        Wi = -(-Nw // 2) * 2				# RGBX rows of 16-byte multiples
        A = self.alloc(Nh * Nw * 64 * 2)
        B = self.alloc(Nh * Nw * 64 * 2)
        sets = []
        for _ in range(2):
            s = dict(IN=self.alloc(Nh * Wi * 4 * 2), in_w=Wi)
            s['OUT'] = self.alloc(Nh * Nw * self.out_c * 2)
            cmds = []
            src, sp, sw = s['IN'], 4, Wi
            for L in self.layers:
                last = L['i'] == compact.N_CONV - 1
                dst, dp = (s['OUT'], self.out_c) if last else ((A, 64) if L['i'] % 2 == 0 else (B, 64))
                cmds.append(self.conv_cmd(L, src, sp, sw, dst, dp))
                src, sp, sw = dst, dp, Nw
            s['cmds'] = cmds
            s['cmd_off'] = self.alloc(len(cmds) * 0x1c0, 4096)
            sets.append(s)
        return sets

    def conv_cmd(self, L, s_off, s_p, s_w, d_off, d_p):
        Nh, Nw, CI, CO = self.Nh, self.Nw, L['ci'], L['co']
        assert d_p * 2 % 16 == 0 and s_w * s_p * 2 % 16 == 0
        th, tw = tiling(Nh, Nw, CI, CO, 3)
        wtile = 2 * mc.wblock(3, ceil2(CI), 2)
        one = CBUF_INPUT * CBUF_BANK + CO // 16 * wtile <= CBUF_TOP * CBUF_BANK
        c = mc.template()
        mc.conv_geometry(c, Nh, Nw, CI, CO, K=3, pad=(1, 1, 1, 1), cpitch=s_p, opitch=d_p,
                         th=th, tw=tw, cout_tiles=not one)
        mc.int16_mode(c)
        if th * tw > 1:
            mc.input_pingpong(c, CBUF_HALF)
        c[0x038] = s_w * s_p
        c[0x000] = s_off
        c[0x02c] = Nw * d_p
        c[0x008] = d_off
        c[0x004] = L['w_off']
        c[0x054] = L['b_off']
        c[0x058] = L['mult']
        c[0x05c] = 6 << 12 | L['sh'] << 8
        c[0x124] = CBUF_INPUT << 16 | CBUF_INPUT if one else cbuf_plan(CI, CO, 3)[1]
        c[0x15c] &= ~(WAIT_PREV | SIGNAL_NEXT)
        if 't_off' in L:
            c[0x070] |= 0x300000
            c[0x0cc] = 0xf0
            c[0x0d0] = 0x0f000000
            c[0x0e0] = 7
            c[0x0e8] = L['ps'] << 17
            c[0x0f0] = L['pm']
            c[0x14c] = 0x18100000
            c[0x16c] = L['t_off']
            c[0x170] = L['t_off'] + 0x400
        return c

    def stage(self, k, tiles):
        """the inputs of a group of tiles into set k (one tile per core)"""
        s_x = float(self.M['s_x'])
        for x8, core in zip(tiles, self.streams):
            s = core[k]
            xin = np.zeros((self.Nh, s['in_w'], 4), '<i2')
            xin[:, :self.Nw, :3] = np.floor(x8.astype(np.float32) * (1 / 255 / s_x) + 0.5).astype(np.int16)
            self.apu.put(s['IN'], xin.tobytes())

    def results(self, k, n):
        """the last conv's int16 outputs of set k's n tiles, with context"""
        size = self.Nh * self.Nw * self.out_c * 2
        return [np.frombuffer(self.apu.get(core[k]['OUT'], size), '<i2').reshape(
            self.Nh, self.Nw, self.out_c) for core in self.streams[:n]]

    def frame(self, img, out=None):
        """uint8 H x W x 3 RGB -> scale x (into out if given)"""
        import threading
        H, W = img.shape[:2]
        (Th, Tw), r, P = self.tile, self.scale, PAD
        ny, nx = -(-H // Th), -(-W // Tw)
        pad = np.pad(img, ((P, P + ny * Th - H), (P, P + nx * Tw - W), (0, 0)), mode='edge')
        if out is None:
            out = np.empty((r * H, r * W, 3), np.uint8)
        s_last = float(self.M['s_last'])
        tiles = [(ty, tx) for ty in range(ny) for tx in range(nx)]
        groups = [tiles[i:i + self.cores] for i in range(0, len(tiles), self.cores)]
        crop = lambda ty, tx: pad[ty * Th:ty * Th + self.Nh, tx * Tw:tx * Tw + self.Nw]

        def finish(k, group):
            for (ty, tx), y in zip(group, self.results(k, len(group))):
                h, w = min(Th, H - ty * Th), min(Tw, W - tx * Tw)
                x = img[ty * Th:ty * Th + h, tx * Tw:tx * Tw + w].astype(np.float32) * (1 / 255)
                f = compact.finish(y[P:P + h, P:P + w, :self.co_last].astype(np.float32) * s_last, x, r)
                out[r * ty * Th:r * (ty * Th + h), r * tx * Tw:r * (tx * Tw + w)] = \
                    np.floor(f * 255 + 0.5).astype(np.uint8)

        # the MDLA runs group g (set g % 2) while the CPU finishes g - 1 and
        # stages g + 1 in the other set
        self.stage(0, [crop(*t) for t in groups[0]])
        for g, group in enumerate(groups):
            k = g % 2
            run = threading.Thread(target=self.apu.run,
                                   args=([core[k]['run'] for core in self.streams[:len(group)]], 30000))
            run.start()
            if g:
                finish(1 - k, groups[g - 1])
            if g + 1 < len(groups):
                self.stage(1 - k, [crop(*t) for t in groups[g + 1]])
            run.join()
        finish((len(groups) - 1) % 2, groups[-1])
        return out

    def close(self):
        self.apu.close()


if __name__ == '__main__':
    # compact_mdla.py model.npz in.npy out.npy [tile]: one frame, timing
    import sys
    model = load(sys.argv[1])
    img = np.load(sys.argv[2])
    T = (int(sys.argv[4]),) * 2 if len(sys.argv) > 4 else pick_tile(*img.shape[:2])
    t = time.monotonic()
    net = Compact(model, tile=T)
    print(f'{T[0]} x {T[1]} px tiles, DMA buffer {net.size / 2 ** 20:.0f} MiB, '
          f'set up in {time.monotonic() - t:.1f} s', flush=True)
    for _ in range(2):
        t = time.monotonic()
        y = net.frame(img)
        print(f'{img.shape[1]}x{img.shape[0]} -> {y.shape[1]}x{y.shape[0]} in {time.monotonic() - t:.2f} s',
              flush=True)
    np.save(sys.argv[3], y)
    net.close()
