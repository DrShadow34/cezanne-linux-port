"""U^2-Net (seg16.py's int16 model, u2netp) on one MDLA core through
/dev/cezanne-apu: the salient object mask of a picture.

    net = U2Net(load('u2netp-int16.npz'))
    mask = net.run(x)            # x: seg16.prepare(rgb), 320 x 320 x 3 float
    net.close()                  # -> 320 x 320 float in [0, 1]

Layout: one DMA buffer: weights, PReLU tables (ReLU = negative factor 0),
commands, activations. Each tensor is an int16 view (pixel pitch P, row
pitch W * P); a concat is one buffer whose inputs are written straight into
its channel slices (a conv reading one of them alone uses the pitch), so
concats cost nothing. Buffers are reused by liveness.

Convs run on the MDLA as x4plus_mdla's (one command per frame, the MDLA's
own spatial tiling, all output channels at once where the weights fit);
a dilated conv (d = 2 / 4 / 8) as d x d standard convs over the decimated
phases (pixel and row pitch x d: zero padding at a phase's edge is the
dilated conv's); where a phase would be under MIN_PHASE px (d 8 on the
10 x 10 stage) the conv runs on the CPU instead.
Side outputs (CO 1) are padded to 16 channels in a buffer of their own (a
CPU copy puts them into a concat). Convs with more than 128 input channels
(the full U^2-Net) come split by seg16.split(): chunk convs read channel
slices of their input, a combine on the CPU sums them. Pixel pitches are multiples of 16 bytes
(the input's RGBX: 8): a 12-byte pitch with 32-byte pixel writes reset the
SoC (2026-11-04). Pool, resize, add, copy and the final sigmoid run in
numpy on views of the DMA buffer (seg16.py's arithmetic), between
submissions: consecutive convs are one submission, chained by the 0x15c
bits."""
import json
import os
import sys
import time

import numpy as np

import mdlacmd as mc
import onnx2ops
import seg16
from x4plus_mdla import (Apu, CBUF_BANK, CBUF_HALF, CBUF_INPUT, CBUF_TOP, IOC_RUN, SIGNAL_NEXT,
                         WAIT_PREV, cbuf_plan, ceil2, tiling)

ALIGN = 256
MIN_PHASE = 2			# smallest decimated phase (rows / columns) on the MDLA


def load(path):
    z = np.load(path)
    meta = json.loads(bytes(z['meta']).decode())
    return meta, {k: z[k] for k in z.files if k != 'meta'}


class View:
    """an int16 tensor in the DMA buffer: H x W x C at byte offset `off` (pixel
    0, its first channel), pixel pitch P elements, row pitch R elements"""
    def __init__(self, buf, c0, C, H, W, P):
        self.buf, self.c0, self.C, self.H, self.W, self.P = buf, c0, C, H, W, P
        self.R = W * P

    @property
    def off(self):
        return self.buf.off + self.c0 * 2

    def at(self, y, x):
        return self.off + (y * self.R + x * self.P) * 2


class Buf:
    def __init__(self, H, W, C, P=None):
        self.H, self.W, self.C = H, W, C
        self.P = P or -(-C // 8) * 8		# 16-byte pixels
        self.size = H * W * self.P * 2
        self.first = self.last = None
        self.off = None


def shapes(ops, H=seg16.SIZE, W=seg16.SIZE):
    hw = {ops[0]['in']: (H, W)}
    ch = {ops[0]['in']: 3}
    for op in ops:
        k, o = op['op'], op['out']
        if k == 'conv':
            hw[o], ch[o] = hw[op['in']], op['co']
        elif k == 'pool':
            h, w = hw[op['in']]
            hw[o], ch[o] = (-(-h // 2), -(-w // 2)), ch[op['in']]
        elif k == 'resize':
            hw[o], ch[o] = hw[op['like']], ch[op['in']]
        elif k == 'concat':
            hw[o], ch[o] = hw[op['ins'][0]], sum(ch[i] for i in op['ins'])
        elif k in ('add', 'sigmoid', 'copy', 'combine'):
            src = op['ins'][0] if k in ('add', 'combine') else op['in']
            hw[o], ch[o] = hw[src], ch[src]
    return hw, ch


def pad16(c):
    return -(-c // 16) * 16


class U2Net:
    def __init__(self, model, apu=None, reuse=True):
        """reuse=False: every tensor keeps its memory (tests: tensor())"""
        meta, P = model
        self.inp, self.outp = meta['input'], meta['output']
        # a conv whose CO is no multiple of 16 writes its own padded buffer;
        # into a concat it goes by a CPU copy
        cat_in = {i for op in meta['ops'] if op['op'] == 'concat' for i in op['ins']}
        ops = []
        for op in meta['ops']:
            if op['op'] == 'conv' and op['co'] % 16 and op['out'] in cat_in:
                ops.append(dict(op, out=op['out'] + '#pad'))
                for k in ('.wq', '.bq', '.rq', '.s'):
                    P[op['out'] + '#pad' + k] = P[op['out'] + k]
                ops.append(dict(op='copy', **{'in': op['out'] + '#pad'}, out=op['out']))
            else:
                ops.append(op)
        self.ops, self.P = ops, P
        hw, ch = shapes(self.ops)
        self.hw, self.ch = hw, ch

        # tensor -> view: concat buffers first, then one buffer per tensor
        views, bufs = {}, []
        for op in self.ops:
            if op['op'] == 'concat':
                h, w = hw[op['out']]
                b = Buf(h, w, ch[op['out']])
                bufs.append(b)
                c0 = 0
                for i in op['ins']:
                    assert i not in views, f'{i} in two concats'
                    views[i] = View(b, c0, ch[i], h, w, b.P)
                    c0 += ch[i]
                views[op['out']] = View(b, 0, b.C, h, w, b.P)
        for name in [self.inp] + [op['out'] for op in self.ops]:
            if name in views:
                continue
            h, w = hw[name]
            C = 4 if name == self.inp else pad16(ch[name])		# RGBX; CO 1 -> 16
            b = Buf(h, w, C, P=4 if name == self.inp else None)
            bufs.append(b)
            views[name] = View(b, 0, ch[name], h, w, b.P)
        self.views = views

        # liveness (op index of the first write / last read), first-fit offsets
        def touch(name, i):
            b = views[name].buf
            b.first = i if b.first is None else min(b.first, i)
            b.last = i if b.last is None else max(b.last, i)
        touch(self.inp, -1)
        for i, op in enumerate(self.ops):
            touch(op['out'], i)
            for src in op.get('ins', [op.get('in')]):
                touch(src, i)
        touch(self.outp, len(self.ops))
        top = 0
        live = []
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

        # weights, biases, PReLU tables after the activations
        self.top = self.act_size
        self.layers = {}
        for op in self.ops:
            if op['op'] != 'conv':
                continue
            o = op['out']
            wq = P[o + '.wq'].astype(np.int16)
            co, k, _, ci = wq.shape
            cop = pad16(co)
            if cop != co:
                wq = np.concatenate([wq, np.zeros((cop - co, k, k, ci), np.int16)])
            blk = mc.wblock(k, ceil2(ci), 2)
            w = mc.pack_conv_weights16(wq, blk)
            b = np.zeros(cop, np.int32)
            b[:co] = P[o + '.bq']
            mult, sh = (int(v) for v in P[o + '.rq'])
            L = dict(op=op, w=w, b=b.tobytes(), mult=mult, sh=sh, co=cop, ci=ci, k=k, blk=blk)
            L['w_off'] = self.alloc(len(L['w']), 256)
            L['b_off'] = self.alloc(len(L['b']), 64)
            if op['relu']:
                L['t_off'] = self.alloc(0x800, 256)
            self.layers[o] = L

        # steps: ('mdla', [commands]) | ('cpu', op)
        self.steps = []
        cmds = []
        for op in self.ops:
            if op['op'] == 'conv' and not self.on_cpu(op):
                cmds += self.conv_cmds(self.layers[op['out']])
            elif op['op'] == 'concat':
                continue
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
            self.apu.put(L['w_off'], L['w'])
            self.apu.put(L['b_off'], L['b'])
            if 't_off' in L:
                self.apu.put(L['t_off'], np.zeros(L['co'], '<i4').tobytes())	# ReLU: negative x 0
                self.apu.put(L['t_off'] + 0x400, np.zeros(L['co'], np.uint8).tobytes())
        off = self.cmd_off
        for s in self.steps:
            if s[0] != 'mdla':
                continue
            cs = s[1]
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
            s[1] = (off, len(cs))
            off += len(cs) * 0x1c0

    def on_cpu(self, op):
        h, w = self.hw[op['in']]
        return op['dil'] > 1 and min(h, w) // op['dil'] < MIN_PHASE

    def alloc(self, n, align=4096):
        self.top = -(-self.top // align) * align
        off = self.top
        self.top += n
        return off

    def src_view(self, op):
        """a conv's input, or the channel slice it reads (seg16.split)"""
        v = self.views[op['in']]
        if 'c0' in op:
            v = View(v.buf, v.c0 + op['c0'], op['ci'], v.H, v.W, v.P)
        return v

    def conv_cmds(self, L):
        op = L['op']
        src, dst = self.src_view(op), self.views[op['out']]
        d = op['dil']
        cmds = []
        for py in range(d):
            for px in range(d):
                H, W = -(-(src.H - py) // d), -(-(src.W - px) // d)
                if H <= 0 or W <= 0:
                    continue
                cmds.append(self.conv_cmd(L, src.at(py, px), src.P * d, src.R * d,
                                          dst.at(py, px), dst.P * d, dst.R * d, H, W))
        return cmds

    def conv_cmd(self, L, s_off, s_p, s_r, d_off, d_p, d_r, H, W):
        op, K, CI, CO = L['op'], L['k'], L['ci'], L['co']
        assert s_p * 2 % 8 == 0 and d_p * 2 % 16 == 0 and d_p >= CO and H >= 2 and W >= 2, op['out']
        th, tw = tiling(H, W, CI, CO, K)
        wtile = 2 * mc.wblock(K, ceil2(CI), 2)
        one = CBUF_INPUT * CBUF_BANK + CO // 16 * wtile <= CBUF_TOP * CBUF_BANK
        assert one or CO <= 16 or cbuf_plan(CI, CO, K), op['out']
        c = mc.template()
        pad = (1, 1, 1, 1) if K == 3 else (0, 0, 0, 0)
        mc.conv_geometry(c, H, W, CI, CO, K=K, pad=pad, cpitch=s_p, opitch=d_p,
                         th=th, tw=tw, cout_tiles=not one)
        mc.int16_mode(c)
        if th * tw > 1:
            mc.input_pingpong(c, CBUF_HALF)
        c[0x038] = s_r
        c[0x000] = s_off
        c[0x02c] = d_r
        c[0x008] = d_off
        c[0x004] = L['w_off']
        c[0x054] = L['b_off']
        c[0x058] = L['mult']
        c[0x05c] = 6 << 12 | L['sh'] << 8
        c[0x124] = CBUF_INPUT << 16 | CBUF_INPUT if one else cbuf_plan(CI, CO, K)[1]
        c[0x15c] &= ~(WAIT_PREV | SIGNAL_NEXT)
        if op['relu']:
            pm, ps = seg16_q31(1.0)
            c[0x070] |= 0x300000
            c[0x0cc] = 0xf0
            c[0x0d0] = 0x0f000000
            c[0x0e0] = 7
            c[0x0e8] = ps << 17
            c[0x0f0] = pm
            c[0x14c] = 0x18100000
            c[0x16c] = L['t_off']
            c[0x170] = L['t_off'] + 0x400
        return c

    # -- CPU side ----------------------------------------------------------
    def arr(self, v):
        """a numpy view of an int16 tensor in the DMA buffer"""
        return np.ndarray((v.H, v.W, v.C), np.int16, buffer=self.apu.mm, offset=v.off,
                          strides=(v.R * 2, v.P * 2, 2))

    def cpu(self, op):
        k, o = op['op'], op['out']
        P, out = self.P, self.views[o]
        if k == 'conv':			# a small dilated one: the integer arithmetic
            c = seg16.Conv.__new__(seg16.Conv)
            c.dil, c.relu = op['dil'], op['relu']
            c.wq, c.bq = P[o + '.wq'].astype(np.int64), P[o + '.bq'].astype(np.int64)
            c.mult, c.sh = (int(v) for v in P[o + '.rq'])
            self.arr(out)[:] = c.run(np.array(self.arr(self.src_view(op)), np.int64))
        elif k == 'combine':
            self.arr(out)[:] = seg16.cpu_combine(
                [np.array(self.arr(self.views[i])) for i in op['ins']],
                [P[i + '.s'][0] for i in op['ins']], P[o + '.bias'], P[o + '.s'][0], op['relu'])
        elif k == 'copy':
            self.arr(out)[:] = self.arr(self.views[op['in']])[..., :out.C]
        elif k == 'pool':
            self.arr(out)[:] = onnx2ops.maxpool(np.array(self.arr(self.views[op['in']])))
        elif k == 'resize':
            self.arr(out)[:] = seg16.cpu_resize(np.array(self.arr(self.views[op['in']])), self.hw[op['like']])
        elif k == 'add':
            a, b = op['ins']
            self.arr(out)[:] = seg16.cpu_add(np.array(self.arr(self.views[a])), P[a + '.s'][0],
                                             np.array(self.arr(self.views[b])), P[b + '.s'][0], P[o + '.s'][0])
        elif k == 'sigmoid':
            x = np.array(self.arr(self.views[op['in']]), np.float64)[..., 0]
            self.mask = 1 / (1 + np.exp(-x * P[op['in'] + '.s'][0]))

    def run(self, x, timing=None, trace=None):
        """x: 320 x 320 x 3 float (seg16.prepare) -> mask 320 x 320 float;
        trace(i, kind, what) before each step (tests)"""
        s_in = self.P[self.inp + '.s'][0]
        q = np.zeros((seg16.SIZE, seg16.SIZE, 4), np.int16)
        q[..., :3] = np.clip(seg16.rnd(x / s_in), -seg16.A - 1, seg16.A)
        self.apu.put(self.views[self.inp].off, q.tobytes())
        t_mdla = t_cpu = 0.0
        for i, (kind, what) in enumerate(self.steps):
            if trace:
                trace(i, kind, what)
            t = time.monotonic()
            if kind == 'mdla':
                self.apu.run([what], 10000)
                t_mdla += time.monotonic() - t
            else:
                self.cpu(what)
                t_cpu += time.monotonic() - t
        if timing is not None:
            timing.update(mdla=t_mdla, cpu=t_cpu)
        return self.mask

    def tensor(self, name):
        """an intermediate result (int16, its real channels)"""
        return np.array(self.arr(self.views[name]))

    def close(self):
        self.apu.close()


def seg16_q31(f):
    """the PReLU stage's factor (base 19), quant16.q31"""
    e = int(np.floor(np.log2(abs(f))))
    s = 30 - 19 - e
    m = int(np.floor(abs(f) * 2.0 ** (19 + s) + 0.5))
    if m >= 1 << 31:
        m >>= 1
        s -= 1
    return m, s


if __name__ == '__main__':
    # u2net_mdla.py model.npz input.npy [ref.npz]: one run, timing; with
    # ref (seg16 tensors of the same input): bit-exact comparison
    model = load(sys.argv[1])
    t = time.monotonic()
    net = U2Net(model, reuse=len(sys.argv) <= 3)
    print(f'{net.ncmd} commands, {len(net.steps)} steps, DMA buffer {net.size / 2 ** 20:.0f} MiB '
          f'(activations {net.act_size / 2 ** 20:.0f}), set up in {time.monotonic() - t:.1f} s', flush=True)
    x = np.load(sys.argv[2])
    log = open('/root/seg-trace.log', 'w')

    def trace(i, kind, what):		# survives a reset of the SoC
        log.write(f'{i} {kind} {what if kind == "mdla" else what["op"] + " " + what["out"]}\n')
        log.flush()
        os.fsync(log.fileno())
    for r in range(2):
        tm = {}
        t = time.monotonic()
        m = net.run(x, tm, trace if r == 0 else None)
        print(f'run {time.monotonic() - t:.3f} s (MDLA {tm["mdla"]:.3f}, CPU {tm["cpu"]:.3f})', flush=True)
    np.save('/tmp/u2net-mask.npy', m)
    if len(sys.argv) > 3:
        ref = np.load(sys.argv[3])
        bad = 0
        for name in ref.files:
            if name == 'mask':
                print(f'mask: max |MDLA - sim| {np.abs(m - ref["mask"]).max():.2e}')
                continue
            a = net.tensor(name)[..., :ref[name].shape[2]]
            d = np.abs(a.astype(np.int64) - ref[name])
            if d.max():
                bad += 1
                print(f'{name}: {int((d > 0).sum())} of {d.size} differ, max {int(d.max())}')
        print('all compared tensors identical' if not bad else f'{bad} tensors differ')
    net.close()
