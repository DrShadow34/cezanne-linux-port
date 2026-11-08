"""MiDaS small (midas.py's int16 model) on one MDLA core through
/dev/cezanne-apu: the relative depth of a picture.

    net = Depth(load('midas-int16.npz'))
    d = net.run(x)               # x: midas.prepare(rgb), 256 x 256 x 3 float
    net.close()                  # -> 256 x 256 float, larger = nearer

Layout as u2net_mdla.py: one DMA buffer with weights, ReLU tables,
commands and int16 activations (pixel pitches multiples of 16 bytes; the
input RGBX), buffers reused by liveness. Convs run as one command each,
the MDLA's own spatial tiling (the input window of To output rows (To - 1)
s + K rows at stride s), all output channels at once where the weights fit.
Stride 2 needs the padded input used up exactly: padding the windows do
not reach is dropped (a leftover padded row made one output row wrong,
re/e23_dw16.py). Depthwise convs (0x070 bit 26, Cin per group 1, weights
[c / 16][tap][16], the vendor's "mode A" words) add no bias in int16:
they write '<out>.nobias' and a CPU step (dwfix) adds the bias, applies
the activation and rounds to the output's scale; a map whose channels do
not fit the input buffer is split into channel chunks (each its own
command on channel slices). ReLU / ReLU6 of the other convs: the PReLU
stage with negative factor 0, the int16 saturation at the scale's top (6
at most) the clamp. Add, resize, relu, combine and dwfix run in numpy
between submissions (midas.py's arithmetic)."""
import functools
import os
import sys
import time

import numpy as np

import mdlacmd as mc
import midas
from u2net_mdla import Buf, View, load, pad16, seg16_q31
from x4plus_mdla import (Apu, CBUF_BANK, CBUF_HALF, CBUF_INPUT, CBUF_TOP, SIGNAL_NEXT, TILE_COST, WAIT_PREV,
                         cbuf_plan, ceil2, ceil4, ceil8)

ALIGN = 256
# a depthwise command's output tile row (Tw px x C channels) at most this:
# up to 4480 ran, from 4608 on it hung or wrote garbage, whatever the input
# window (re/e24_dwtile.py; the vendor's tiles stay far below)
DW_ROW = 4096
# all output channels at once (the weights of the whole layer in the CBUF):
# up to 512 channels ran (with up to 110 KB of weights), 576 hung even with
# 55 KB (re/e25_allco.py); more: 16-channel output tiles
ALLCO_MAX = 512


def shapes(ops, H=midas.SIZE, W=midas.SIZE):
    hw = {ops[0]['in']: (H, W)}
    ch = {ops[0]['in']: 3}
    for op in ops:
        k, o = op['op'], op['out']
        if k == 'conv':
            h, w = hw[op['in']]
            t, b, l, r = op['pad']
            s, K = op['stride'], op['k']
            hw[o], ch[o] = ((h + t + b - K) // s + 1, (w + l + r - K) // s + 1), op['co']
        elif k == 'resize':
            h, w = hw[op['in']]
            hw[o], ch[o] = (2 * h, 2 * w), ch[op['in']]
        else:			# relu, add, combine, dwfix
            src = op['ins'][0] if 'ins' in op else op['in']
            hw[o], ch[o] = hw[src], ch[src]
    return hw, ch


@functools.lru_cache(maxsize=None)
def tiling(Ho, Wo, CI, K, s, macs_px, dw=False):
    """spatial tiles (th, tw) for one command (x4plus_mdla.tiling at stride
    s): each tile's input window, rows and columns rounded up to 8, fits
    one input buffer (half the input banks); depthwise: Tw x C <= DW_ROW;
    None if no tiling fits"""
    room = CBUF_HALF * CBUF_BANK // (ceil2(CI) * 2)
    best = None
    for th in range(1, min(Ho, 255) + 1):
        To = -(-Ho // th)
        if th * To - Ho >= To or ceil8((To - 1) * s + K) * 8 > room:
            continue
        rows = (th - 1) * ceil4(To) + ceil4(Ho - (th - 1) * To)
        for tw in range(1, min(Wo, 255) + 1):
            if best and th * tw * TILE_COST + Ho * Wo * macs_px >= best[0]:
                break
            Tw = -(-Wo // tw)
            if tw * Tw - Wo >= Tw or ceil8((To - 1) * s + K) * ceil8((Tw - 1) * s + K) > room \
                    or dw and Tw * CI > DW_ROW:
                continue
            cols = (tw - 1) * ceil8(Tw) + ceil8(Wo - (tw - 1) * Tw)
            cost = th * tw * TILE_COST + rows * cols * macs_px
            if best is None or cost < best[0]:
                best = (cost, th, tw)
    return None if best is None else best[1:]


def geometry(c, H, W, CI, CO, K, pad, s, cp, op, th, tw, dw):
    """mdlacmd.conv_geometry at stride s (re/mdla.py's fields, the vendor's
    strided commands): pad (top, bottom, left, right) with the unreached
    padding dropped"""
    t, b, l, r = pad
    b -= (H + t + b - K) % s
    r -= (W + l + r - K) % s
    assert b >= 0 and r >= 0
    Ho, Wo = (H + t + b - K) // s + 1, (W + l + r - K) // s + 1
    To, Tw = -(-Ho // th), -(-Wo // tw)
    Eh, Ew = th * To - Ho, tw * Tw - Wo
    assert Eh < To and Ew < Tw, 'an empty last tile hangs the MDLA'
    c[0x00c] = H << 16 | 1
    c[0x010] = W
    c[0x014] = CI * 16
    c[0x018] = K << 16 | CO
    c[0x01c] = (1 if dw else CI) << 16 | K
    c[0x020] = Ho << 16 | 1
    c[0x024] = CO << 16 | Wo
    c[0x028] = Ho * Wo * op
    c[0x02c] = Wo * op
    c[0x030] = op
    c[0x034] = H * W * cp
    c[0x038] = W * cp
    c[0x03c] = cp
    c[0x04c] = -(-K * K * CI * 2 // 256) * 256 if dw else mc.wblock(K, ceil2(CI), 2)
    c[0x07c] = ((To - 1) * s + K) << 16 | ((Tw - 1) * s + K)
    c[0x080] = (To * s) << 16 | (Tw * s)
    c[0x084] = (Eh * s + b) << 16 | t
    c[0x088] = (Ew * s + r) << 16 | l
    c[0x08c] = c[0x09c] = To << 16 | Tw
    c[0x094] = c[0x0a0] = Eh << 16
    c[0x098] = c[0x0a4] = Ew << 16
    c[0x0c0] = s << 4 | s
    c[0x0c4] = (c[0x0c4] & 0xff0000) | b << 12 | t << 8 | r << 4 | l
    c[0x19c] = H * W * cp * 2
    c[0x1a4] = Ho * Wo * op * 2
    return Ho, Wo


def dw_chunks(C, H, W, K, s, pad):
    """channel chunks (multiples of 16) of a depthwise conv whose inputs
    fit the input buffer with some tiling"""
    t, b, l, r = pad
    Ho, Wo = (H + t + b - K) // s + 1, (W + l + r - K) // s + 1
    for n in range(1, C // 16 + 1):
        size = -(-C // n // 16) * 16
        if tiling(Ho, Wo, size, K, s, size * K * K, True):
            return [(c0, min(size, C - c0)) for c0 in range(0, C, size)]
    raise ValueError(f'no depthwise tiling for {C} x {H} x {W}')


class Depth:
    def __init__(self, model, apu=None, reuse=True, single=False):
        """single: one submission per conv (tests: check() after each)"""
        meta, P = model
        self.inp, self.outp = meta['input'], meta['output']
        ops = []
        for op in meta['ops']:			# depthwise: the MDLA conv + dwfix
            if op['op'] == 'conv' and op['dw']:
                o = op['out']
                ops.append(dict(op, out=o + '.nobias', act='none'))
                for k in ('.wq', '.bq', '.rq'):
                    P[o + '.nobias' + k] = P[o + k]
                ops.append(dict(op='dwfix', **{'in': o + '.nobias'}, out=o, act=op['act']))
            else:
                ops.append(op)
        self.ops, self.P = ops, P
        self.S = {k[:-2]: float(v[0]) for k, v in P.items() if k.endswith('.s')}
        hw, ch = shapes(ops)
        self.hw, self.ch = hw, ch

        views, bufs = {}, []
        for name in [self.inp] + [op['out'] for op in ops]:
            h, w = hw[name]
            C = 4 if name == self.inp else pad16(ch[name])		# RGBX; CO 1 / 24 / 136 -> 16s
            b = Buf(h, w, C, P=4 if name == self.inp else None)
            bufs.append(b)
            views[name] = View(b, 0, ch[name], h, w, b.P)
        self.views = views

        def touch(name, i):
            b = views[name].buf
            b.first = i if b.first is None else min(b.first, i)
            b.last = i if b.last is None else max(b.last, i)
        touch(self.inp, -1)
        for i, op in enumerate(ops):
            touch(op['out'], i)
            for src in op.get('ins', [op.get('in')]):
                touch(src, i)
        touch(self.outp, len(ops))
        top, live = 0, []
        for b in sorted(bufs, key=lambda b: b.first):
            live = [x for x in live if x.last >= b.first or not reuse]
            off = 0
            for x in sorted(live, key=lambda x: x.off):
                if off + b.size <= x.off:
                    break
                off = max(off, -(-(x.off + x.size) // ALIGN) * ALIGN)
            b.off = off
            live.append(b)
            top = max(top, off + b.size)
        self.act_size = -(-top // 4096) * 4096

        self.top = self.act_size
        self.layers = {}
        for op in ops:
            if op['op'] != 'conv':
                continue
            self.layers[op['out']] = self.layer(op)

        self.steps, cmds = [], []
        for op in ops:
            if op['op'] == 'conv' and single:
                if cmds:
                    self.steps.append(['mdla', cmds])
                self.steps.append(['mdla', self.conv_cmds(self.layers[op['out']])])
                self.steps[-1].append(op)
                cmds = []
            elif op['op'] == 'conv':
                cmds += self.conv_cmds(self.layers[op['out']])
            else:
                if cmds:
                    self.steps.append(['mdla', cmds])
                    cmds = []
                self.steps.append(['cpu', op])
        if cmds:
            self.steps.append(['mdla', cmds])
        self.ncmd = sum(len(s[1]) for s in self.steps if s[0] == 'mdla')
        self.cmd_off = self.alloc(self.ncmd * 0x1c0, 4096)
        self.size = -(-self.top // 4096) * 4096

        self.apu = apu or Apu(self.size)
        base = self.apu.base
        for L in self.layers.values():
            for w_off, w in L['w']:
                self.apu.put(w_off, w)
            self.apu.put(L['b_off'], L['b'])
            if 't_off' in L:
                self.apu.put(L['t_off'], np.zeros(L['co'], '<i4').tobytes())	# ReLU: negative x 0
                self.apu.put(L['t_off'] + L['t_shift'], np.zeros(L['co'], np.uint8).tobytes())
        off = self.cmd_off
        for st in self.steps:
            if st[0] != 'mdla':
                continue
            cs = st[1]
            for i, c in enumerate(cs):
                for o in (0x000, 0x004, 0x008, 0x054):
                    c[o] = base + c[o]
                if c[0x16c]:
                    c[0x16c] = base + c[0x16c]
                    c[0x170] = base + c[0x170]
                c[0x15c] |= (WAIT_PREV if i else 0) | (SIGNAL_NEXT if i < len(cs) - 1 else 0)
                c[0x150] = i + 1
                c[0x154] = (c[0x154] & ~(1 << 24)) | (1 << 24 if i == len(cs) - 1 else 0)
            self.apu.put(off, b''.join(c.bytes() for c in cs))
            st[1] = (off, len(cs))
            off += len(cs) * 0x1c0

    def alloc(self, n, align=4096):
        self.top = -(-self.top // align) * align
        off = self.top
        self.top += n
        return off

    def layer(self, op):
        """packed weights (depthwise: per channel chunk), bias, ReLU table"""
        o = op['out']
        P = self.P
        wq = P[o + '.wq'].astype(np.int16)
        co, k, _, ci = wq.shape
        cop = pad16(co)
        L = dict(op=op, co=cop, k=k, mult=int(P[o + '.rq'][0]), sh=int(P[o + '.rq'][1]))
        if op['dw']:
            src = self.views[op['in']]
            L['chunks'] = dw_chunks(co, src.H, src.W, k, op['stride'], tuple(op['pad']))
            L['ci'] = co
            L['w'] = []
            for c0, n in L['chunks']:
                a = wq[c0:c0 + n, :, :, 0].reshape(n // 16, 16, k * k).transpose(0, 2, 1)
                w = np.ascontiguousarray(a).astype('<i2').tobytes()
                w += b'\0' * (-(-len(w) // 256) * 256 - len(w))
                L['w'].append((self.alloc(len(w), 256), w))
        else:
            if cop != co:
                wq = np.concatenate([wq, np.zeros((cop - co, k, k, ci), np.int16)])
            L['ci'] = ci
            w = mc.pack_conv_weights16(wq, mc.wblock(k, ceil2(ci), 2))
            L['w'] = [(self.alloc(len(w), 256), w)]
        b = np.zeros(cop, np.int32)
        b[:co] = P[o + '.bq']
        L['b'] = b.tobytes()
        L['b_off'] = self.alloc(len(L['b']), 64)
        if op['act'] != 'none' and not op['dw']:		# ReLU: int32 factors, uint8 shifts
            L['t_shift'] = -(-cop * 4 // 256) * 256
            L['t_off'] = self.alloc(L['t_shift'] + cop, 256)
        return L

    def src_view(self, op):
        v = self.views[op['in']]
        if 'c0' in op:
            v = View(v.buf, v.c0 + op['c0'], op['ci'], v.H, v.W, v.P)
        return v

    def conv_cmds(self, L):
        op = L['op']
        src, dst = self.src_view(op), self.views[op['out']]
        if not op['dw']:
            return [self.conv_cmd(L, src.off, src.P, dst.off, dst.P, src.H, src.W, L['ci'], L['co'],
                                  L['w'][0][0])]
        cmds = []
        for (c0, n), (w_off, _) in zip(L['chunks'], L['w']):
            cmds.append(self.conv_cmd(L, src.off + c0 * 2, src.P, dst.off + c0 * 2, dst.P, src.H, src.W,
                                      n, n, w_off))
        return cmds

    def conv_cmd(self, L, s_off, s_p, d_off, d_p, H, W, CI, CO, w_off):
        op, K, s, dw = L['op'], L['k'], op_stride(L), L['op']['dw']
        assert s_p * 2 % 8 == 0 and d_p * 2 % 16 == 0 and d_p >= CO and W * s_p * 2 % 16 == 0, op['out']
        t, b, l, r = op['pad']
        Ho, Wo = (H + t + b - K) // s + 1, (W + l + r - K) // s + 1
        macs = CO * K * K if dw else CO * CI * K * K
        th, tw = tiling(Ho, Wo, CI, K, s, macs, dw)
        if dw:
            one = True
        else:
            wtile = 2 * mc.wblock(K, ceil2(CI), 2)
            one = CBUF_INPUT * CBUF_BANK + CO // 16 * wtile <= CBUF_TOP * CBUF_BANK and CO <= ALLCO_MAX
            assert one or CO <= 16 or cbuf_plan(CI, CO, K), op['out']
        c = mc.template()
        geometry(c, H, W, CI, CO, K, op['pad'], s, s_p, d_p, th, tw, dw)
        ct = 1 if one else CO // 16
        c[0x074] = 1 << 24 | ct << 16 | th << 8 | tw
        c[0x078] = ct * th * tw
        if one:
            c[0x090] = (CO << 16 if dw else 1 << 16) | CO
            c[0x0c8] &= ~0x400
        mc.int16_mode(c)
        if dw:
            c[0x070] |= 0x04000000
            c[0x0a8] = 0x33333333
            c[0x0bc] = 0x00f00002
            c[0x148] = 0x80
        if th * tw > 1:
            mc.input_pingpong(c, CBUF_HALF)
        c[0x000] = s_off
        c[0x004] = w_off
        c[0x008] = d_off
        c[0x054] = L['b_off']
        c[0x058] = L['mult']
        c[0x05c] = (c[0x05c] & ~0xf00) | L['sh'] << 8
        c[0x124] = CBUF_INPUT << 16 | CBUF_INPUT if one else cbuf_plan(CI, CO, K)[1]
        c[0x15c] &= ~(WAIT_PREV | SIGNAL_NEXT)
        if 't_off' in L:
            pm, ps = seg16_q31(1.0)
            c[0x070] |= 0x300000
            c[0x0cc] = 0xf0
            c[0x0d0] = 0x0f000000
            c[0x0e0] = 7
            c[0x0e8] = ps << 17
            c[0x0f0] = pm
            c[0x14c] = 0x18100000
            c[0x16c] = L['t_off']
            c[0x170] = L['t_off'] + L['t_shift']
        return c

    # -- CPU side ----------------------------------------------------------
    def arr(self, v):
        return np.ndarray((v.H, v.W, v.C), np.int16, buffer=self.apu.mm, offset=v.off,
                          strides=(v.R * 2, v.P * 2, 2))

    def get(self, name):
        return np.array(self.arr(self.views[name]))

    def cpu(self, op):
        k, o = op['op'], op['out']
        out, S, P = self.arr(self.views[o]), self.S, self.P
        if k == 'dwfix':
            out[:] = midas.cpu_dwfix(self.get(op['in']), S[op['in']], P[o + '.bias'], S[o], op['act'])
        elif k == 'relu':
            out[:] = np.maximum(self.get(op['in']), 0)
        elif k == 'add':
            a, b = op['ins']
            out[:] = midas.cpu_add(self.get(a), S[a], self.get(b), S[b], S[o])
        elif k == 'resize':
            out[:] = midas.cpu_resize(self.get(op['in']))
        elif k == 'combine':
            out[:] = midas.cpu_combine([self.get(p) for p in op['ins']], [S[p] for p in op['ins']],
                                       P[o + '.bias'], S[o], op['act'])

    def run(self, x, timing=None, trace=None, check=None):
        """x: midas.prepare() 256 x 256 x 3 -> relative inverse depth (float);
        check(step) after each step (False: stop)"""
        q = np.zeros((midas.SIZE, midas.SIZE, 4), np.int16)
        q[..., :3] = np.clip(midas.rnd(x / self.S[self.inp]), -midas.A - 1, midas.A)
        self.apu.put(self.views[self.inp].off, q.tobytes())
        t_mdla = t_cpu = 0.0
        for i, (kind, what, *_) in enumerate(self.steps):
            if trace:
                trace(i, kind, what)
            t = time.monotonic()
            if kind == 'mdla':
                self.apu.run([what], 10000)
                t_mdla += time.monotonic() - t
            else:
                self.cpu(what)
                t_cpu += time.monotonic() - t
            if check and check(self.steps[i]) is False:
                return None
        if timing is not None:
            timing.update(mdla=t_mdla, cpu=t_cpu)
        return self.get(self.outp)[..., 0] * self.S[self.outp]

    def close(self):
        self.apu.close()


def op_stride(L):
    return L['op']['stride']


if __name__ == '__main__':
    # depth_mdla.py model.npz input.npy [ref.npz]: one run, timing; with
    # ref (midas.qforward tensors of the same input): exact comparison
    model = load(sys.argv[1])
    t = time.monotonic()
    debug = len(sys.argv) > 3
    net = Depth(model, reuse=not debug, single=debug)
    ref = np.load(sys.argv[3]) if debug else None
    print(f'{net.ncmd} commands, {len(net.steps)} steps, DMA buffer {net.size / 2 ** 20:.0f} MiB '
          f'(activations {net.act_size / 2 ** 20:.0f}), set up in {time.monotonic() - t:.1f} s', flush=True)
    x = np.load(sys.argv[2])
    log = open('/root/depth-trace.log', 'w')

    def trace(i, kind, what):		# survives a reset of the SoC
        st = net.steps[i]
        log.write(f'{i} {kind} {what if kind == "mdla" else what["op"] + " " + what["out"]}'
                  f'{" " + st[2]["out"] if kind == "mdla" and len(st) > 2 else ""}\n')
        log.flush()
        os.fsync(log.fileno())
    def check(step):
        """the step's result against the simulation: stop at the first
        tensor more than 1 off"""
        op = step[2] if step[0] == 'mdla' else step[1]
        name = op['out']
        if name not in ref.files:
            return True
        a = net.get(name).astype(np.int64)
        dd = np.abs(a - ref[name])
        worst.append((int(dd.max()) / max(int(np.abs(ref[name]).max()), 1), name))
        if dd.max() > max(4, 0.01 * np.abs(ref[name]).max()):	# 1-LSB ties grow a little
            i = np.unravel_index(np.argmax(dd), dd.shape)
            print(f'FIRST DIFFERENCE {op["op"]} {name} ({op.get("k")} s{op.get("stride")} dw {op.get("dw")}): '
                  f'{int((dd > 1).sum())} of {dd.size} off by more than 1, max {int(dd.max())} at {i}', flush=True)
            return False
        return True
    worst = []
    for r in range(1 if debug else 2):
        tm = {}
        t = time.monotonic()
        try:
            d = net.run(x, tm, trace if r == 0 else None, check if debug else None)
        except TimeoutError:
            print('TIMEOUT, last step in /root/depth-trace.log', flush=True)
            d = None
        if d is None:
            break
        print(f'run {time.monotonic() - t:.3f} s (MDLA {tm["mdla"]:.3f}, CPU {tm["cpu"]:.3f})', flush=True)
    if worst:
        print('largest differences (of the tensor range):', [(n, round(v, 4)) for v, n in sorted(worst)[-5:]])
    if d is not None:
        np.save('/tmp/depth.npy', d)
    if debug and d is not None:
        bad = 0
        for name in ref.files:
            if name == 'depth':
                print(f'depth: max |MDLA - sim| {np.abs(d - ref["depth"]).max() / ref["depth"].max():.2e} of the range')
                continue
            a = net.get(name)
            dd = np.abs(a.astype(np.int64) - ref[name])
            if dd.max() > 1:
                bad += 1
                print(f'{name}: {int((dd > 1).sum())} of {dd.size} differ by more than 1, max {int(dd.max())}')
        print('all compared tensors within 1' if not bad else f'{bad} tensors differ', flush=True)
    net.close()
