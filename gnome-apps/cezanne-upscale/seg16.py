"""int16 quantisation of U^2-Net (onnx2ops.py's op list) for the MDLA, and
its exact integer simulation (as quant16.py does for x4plus):

- activations int16 symmetric, one calibrated scale per tensor (x RANGE);
  a concat only places its inputs side by side, each keeps its scale
- conv weights int16 per tensor with the input channels' scales folded in,
  bounded so the largest calibrated accumulator * MARGIN stays below 2^31;
  requant with the Q31 multiplier (y = acc * mult / 2^(31 + sh)); ReLU as
  the PReLU stage with negative factor 0; dilation d as d x d decimated
  standard convs (the runner), the same numbers
- pool: max of int16; resize (bilinear) and add: in float64 on the CPU,
  rounded half up to the output's scale (the runner runs the same code)
- only what the fused output d0 needs; sigmoid of d0 in float
- convs with more than CI_CHUNK input channels (full U^2-Net: up to 1024;
  int16 3x3 weights of more do not fit the CBUF) as chunk convs over
  channel slices (no bias, no ReLU, each partial its own scale) and a
  combine on the CPU: the partials and the bias summed in float64, rounded
  to the output's scale, ReLU (split())

    seg16.py calibrate f32.npz ranges.json photo...    (dev: the ranges)
    seg16.py export f32.npz ranges.json int16.npz       (the package build)
    seg16.py compare f32.npz ranges.json photo...       (dev: int16 vs float)
"""
import json
import sys

import numpy as np

import onnx2ops

A = 32767
CI_CHUNK = 128			# input channels per MDLA conv (u2netp's largest)
MARGIN = 4.0
RANGE = 1.25
SIZE = 320
MEAN = np.array([0.485, 0.456, 0.406], np.float32)
STD = np.array([0.229, 0.224, 0.225], np.float32)


def rnd(v):
    return np.floor(v + 0.5)


def load(path):
    z = np.load(path)
    meta = json.loads(bytes(z['ops']).decode())
    weights = {k: z[k] for k in z.files if k != 'ops'}
    return meta, weights


def prune(meta):
    """the ops the first output (d0) needs, in order"""
    need = {meta['outputs'][0]}
    keep = []
    for op in reversed(meta['ops']):
        if op['out'] not in need:
            continue
        keep.append(op)
        for i in op.get('ins', [op.get('in')]):
            need.add(i)
        if op['op'] == 'resize':
            need.add(op['like'])
    return keep[::-1]


def normalise(rgb320):
    """uint8 320 x 320 x 3 -> the network's input (rembg's: / max, ImageNet
    normalisation), float"""
    x = rgb320.astype(np.float32)
    x = x / max(x.max(), 1)
    return (x - MEAN) / STD


def split(ops, weights):
    """convs with more than CI_CHUNK input channels -> chunk convs over
    channel slices (c0 / ci; no bias, no ReLU) + a combine (sum + bias +
    ReLU on the CPU); float weights split alike"""
    out, w = [], dict(weights)
    for op in ops:
        if op['op'] != 'conv' or op['ci'] <= CI_CHUNK:
            out.append(op)
            continue
        o = op['out']
        W, b = w.pop(o + '.w'), w.pop(o + '.b')
        parts = []
        for k, c0 in enumerate(range(0, op['ci'], CI_CHUNK)):
            n = min(CI_CHUNK, op['ci'] - c0)
            name = f'{o}#p{k}'
            out.append(dict(op, out=name, c0=c0, ci=n, relu=False))
            w[name + '.w'] = np.ascontiguousarray(W[..., c0:c0 + n])
            w[name + '.b'] = np.zeros_like(b)
            parts.append(name)
        out.append(dict(op='combine', ins=parts, out=o, relu=op['relu']))
        w[o + '.bias'] = b
    return out, w


def q16(r):
    """float32 (overwritten) -> int16 range, floor(v + 0.5); the CPU steps
    are float32 (3x faster than float64 on the phone, results as the
    simulation's since both are this code)"""
    r += np.float32(0.5)
    np.floor(r, out=r)
    np.clip(r, -A - 1, A, out=r)
    return r.astype(np.int32)


def cpu_combine(parts, scales, bias, s_out, relu):
    """int16 partials at their scales + the float bias -> int16 at s_out"""
    acc = (bias / s_out).astype(np.float32)
    for p, sc in zip(parts, scales):
        acc = acc + p.astype(np.float32) * np.float32(sc / s_out)
    y = q16(acc)
    return np.maximum(y, 0) if relu else y


def prepare(rgb):
    """uint8 H x W x 3 -> normalise() of it at 320 x 320 (PIL Lanczos, as
    rembg; the camera helper does this with OpenCV)"""
    from PIL import Image
    return normalise(np.asarray(Image.fromarray(rgb).resize((SIZE, SIZE), Image.LANCZOS)))


class Conv:
    """integer parameters of one conv: wq [co][k][k][ci], bq, mult / sh, relu"""
    def __init__(self, op, s_in, w, b, rng):
        self.op = op
        self.dil, self.relu = op['dil'], op['relu']
        w_eff = w.astype(np.float64) * s_in[None, None, None, :]
        self.s_y = RANGE * rng / A
        s_w = max(np.abs(w_eff).max() / A, RANGE * rng * MARGIN / 2 ** 31)
        self.wq = rnd(w_eff / s_w).astype(np.int64)
        self.bq = rnd(b / s_w).astype(np.int64)
        f = s_w / self.s_y
        self.sh = max(0, min(15, int(np.floor(-np.log2(f)))))
        self.mult = int(rnd(f * 2.0 ** (31 + self.sh)))
        assert self.mult < 1 << 31 and np.abs(self.bq).max() < 2 ** 31, op['out']

    def run(self, x):
        acc = onnx2ops.conv(x.astype(np.float64), self.wq.astype(np.float64), 0, self.dil)
        acc = np.clip(acc.astype(np.int64) + self.bq, -2 ** 31, 2 ** 31 - 1)
        S = 31 + self.sh
        y = np.clip((acc * self.mult + (1 << (S - 1))) >> S, -A - 1, A)
        return np.maximum(y, 0) if self.relu else y


def cpu_resize(x, size):
    return q16(onnx2ops.bilinear(x.astype(np.float32), size))


def cpu_add(a, sa, b, sb, sy):
    r = a.astype(np.float32) * np.float32(sa / sy)
    r += b.astype(np.float32) * np.float32(sb / sy)
    return q16(r)


def calibrate(ops, weights, images):
    """max |value| per tensor over the images"""
    rng = {}

    def seen(k, v):
        rng[k] = max(rng.get(k, 0.0), float(np.abs(v).max()))
    for x in images:
        seen(ops[0]['in'], x)
        onnx2ops.forward(ops, weights, x, seen, keep=False)
    return rng


def quantise(ops, weights, rng):
    """-> (convs {out: Conv}, scales {tensor: scale per channel})"""
    first = ops[0]['in']
    s = {first: np.full(3, RANGE * rng[first] / A)}
    convs = {}
    for op in ops:
        k, o = op['op'], op['out']
        if k == 'conv':
            c0 = op.get('c0', 0)
            c = Conv(op, s[op['in']][c0:c0 + op['ci']], weights[o + '.w'], weights[o + '.b'], rng[o])
            convs[o] = c
            s[o] = np.full(op['co'], c.s_y)
        elif k == 'combine':
            s[o] = np.full(len(s[op['ins'][0]]), RANGE * rng[o] / A)
        elif k in ('pool', 'resize'):
            s[o] = s[op['in']]
        elif k == 'concat':
            s[o] = np.concatenate([s[i] for i in op['ins']])
        elif k == 'add':
            s[o] = np.full(len(s[op['ins'][0]]), RANGE * rng[o] / A)
        elif k == 'sigmoid':
            s[o] = s[op['in']]
    return convs, s


def qforward(ops, convs, s, x, bias=None, keep=True):
    """the integer network on a float input -> d0 (float, after the
    sigmoid); bias: the combines' float biases ({out: array})"""
    first = ops[0]['in']
    t = {first: np.clip(rnd(x / s[first]), -A - 1, A).astype(np.int64)}
    last = onnx2ops.last_uses(ops) if not keep else None
    for i, op in enumerate(ops):
        k, o = op['op'], op['out']
        if k == 'conv':
            x_in = t[op['in']]
            if 'c0' in op:
                x_in = x_in[..., op['c0']:op['c0'] + op['ci']]
            t[o] = convs[o].run(x_in)
        elif k == 'combine':
            t[o] = cpu_combine([t[i] for i in op['ins']], [s[i][0] for i in op['ins']],
                               bias[o], s[o][0], op['relu'])
        elif k == 'pool':
            t[o] = onnx2ops.maxpool(t[op['in']])
        elif k == 'resize':
            t[o] = cpu_resize(t[op['in']], t[op['like']].shape[:2])
        elif k == 'concat':
            t[o] = np.concatenate([t[i] for i in op['ins']], axis=2)
        elif k == 'add':
            a, b = op['ins']
            t[o] = cpu_add(t[a], s[a][0], t[b], s[b][0], s[o][0])
        elif k == 'sigmoid':
            t[o] = 1 / (1 + np.exp(-t[op['in']] * s[op['in']][0]))
        if last is not None:
            for name in [n for n in t if last.get(n, -1) <= i and n != o]:
                del t[name]
    return t[ops[-1]['out']][..., 0], t


def biases(ops, weights):
    return {op['out']: weights[op['out'] + '.bias'] for op in ops if op['op'] == 'combine'}


def export(ops, convs, s, dst, bias):
    out = {'meta': np.frombuffer(json.dumps(dict(ops=ops, input=ops[0]['in'],
                                                 output=ops[-1]['out'])).encode(), np.uint8)}
    for o, b in bias.items():
        out[o + '.bias'] = b.astype(np.float64)
    for o, c in convs.items():
        out[o + '.wq'] = c.wq.astype(np.int16)
        out[o + '.bq'] = c.bq.astype(np.int32)
        out[o + '.rq'] = np.array([c.mult, c.sh], np.int64)
    for name, sc in s.items():
        out[name + '.s'] = sc.astype(np.float64)
    np.savez(dst, **out)


def main():
    cmd, src, ranges, *rest = sys.argv[1:]
    meta, weights = load(src)
    ops, weights = split(prune(meta), weights)
    bias = biases(ops, weights)
    if cmd == 'export':
        with open(ranges) as f:
            rng = json.load(f)
        export(ops, *quantise(ops, weights, rng), rest[0], bias)
        return
    from PIL import Image
    imgs = [np.asarray(Image.open(p).convert('RGB')) for p in rest]
    xs = [prepare(im) for im in imgs]
    if cmd == 'calibrate':
        xs += [prepare(im[:, ::-1].copy()) for im in imgs]	# mirrored too
        rng = calibrate(ops, weights, xs)
        with open(ranges, 'w') as f:
            json.dump({k: round(v, 6) for k, v in rng.items()}, f, indent=0)
        print(len(rng), 'ranges ->', ranges)
        return
    with open(ranges) as f:
        rng = json.load(f)
    convs, s = quantise(ops, weights, rng)
    worst = 0
    for i, x in enumerate(xs):
        ref = onnx2ops.forward(ops, weights, x, keep=False)[ops[-1]['out']][..., 0]
        q, _ = qforward(ops, convs, s, x, bias, keep=False)
        d = np.abs(q - ref)
        iou = ((q > .5) & (ref > .5)).sum() / max(((q > .5) | (ref > .5)).sum(), 1)
        worst = max(worst, d.max())
        print(f'photo {i}: max |int16 - float| {d.max():.4f}, mean {d.mean():.5f}, IoU@0.5 {iou:.4f}')
    print('worst', worst)


if __name__ == '__main__':
    main()
