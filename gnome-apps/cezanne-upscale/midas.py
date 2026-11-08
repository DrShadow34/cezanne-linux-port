"""MiDaS v2.1 small (monocular depth, MIT; the EfficientNet-Lite3 encoder
with depthwise convs, the RefineNet decoder) for the MDLA: ONNX -> a flat op
list with batch norm folded, its float reference (checked against
onnxruntime), int16 quantisation and the exact integer simulation the
runner (depth_mdla.py) reproduces.

    midas.py check model-small.onnx [photo...]             (dev: numpy vs onnxruntime)
    midas.py calibrate model-small.onnx ranges.json photo... (dev: the ranges)
    midas.py export model-small.onnx ranges.json out.npz     (the package build)
    midas.py compare model-small.onnx ranges.json photo...   (dev: int16 vs float)

Ops (tensors NHWC float, batch 1, by name):
    conv    in, out, k (1 / 3 / 5), stride, pad (top, bottom, left, right),
            ci, co, dw (depthwise: ci == co), act (none / relu / relu6);
            weights w [co][k][k][ci] (depthwise [c][k][k][1]), b
    relu    in, out             (the RefineNet units' pre-activation)
    add     ins, out
    resize  in, out             (2x bilinear, ONNX opset 10: asymmetric,
                                 source = destination / 2)
The input is the normalised picture (ImageNet mean / std, the model's own
Sub / Div), 256 x 256; the output (the last conv's, ReLU) is relative
inverse depth: larger = nearer.

int16 (seg16.py's conventions): one calibrated scale per tensor (x RANGE),
weights per tensor with the input channels' scales folded in, Q31 requant;
ReLU6 outputs at a scale whose int16 top is 6 at most, so the requant's
saturation is the clamp. Depthwise convs run without bias on the MDLA (an
int16 depthwise command adds none, re/e23_dw16.py): their output at the
calibrated scale of the conv without bias, then the CPU adds the bias,
applies the activation and rounds to the output's scale (dwfix). Convs with
more than CI_CHUNK input channels (3 x 3 only: the decoder's 512 / 256) as
chunk convs + a CPU combine, as seg16.split."""
import json
import sys

import numpy as np

A = 32767
RANGE = 1.25
MARGIN = 4.0
SIZE = 256
CI_CHUNK = 128			# 3 x 3 input channels per MDLA conv
MEAN = np.array([0.485, 0.456, 0.406], np.float32)
STD = np.array([0.229, 0.224, 0.225], np.float32)


def rnd(v):
    return np.floor(v + 0.5)


# -- the graph ---------------------------------------------------------------

def convert(path):
    """-> (input name, ops, weights {name: array}, output name)"""
    import onnx
    from onnx import numpy_helper
    m = onnx.load(path)
    g = m.graph
    init = {t.name: numpy_helper.to_array(t) for t in g.initializer}
    for n in g.node:
        if n.op_type == 'Constant':
            init[n.output[0]] = numpy_helper.to_array(n.attribute[0].t)
    users = {}
    for n in g.node:
        for i in n.input:
            users.setdefault(i, []).append(n)
    attrs = lambda n: {a.name: onnx.helper.get_attribute_value(a) for a in n.attribute}
    alias, pads, fused = {}, {}, set()
    ops, weights = [], {}
    inp = out = None

    def only_user(name, kind):
        us = users.get(name, [])
        return us[0] if len(us) == 1 and us[0].op_type == kind else None
    for n in g.node:
        if id(n) in fused or n.op_type == 'Constant':
            continue
        a = attrs(n)
        ins = [alias.get(i, i) for i in n.input]
        if n.op_type == 'Sub':			# the input normalisation: done outside
            assert np.allclose(init[n.input[1]].ravel(), MEAN)
            alias[n.output[0]] = ins[0]
        elif n.op_type == 'Div':
            assert np.allclose(init[n.input[1]].ravel(), STD)
            inp = n.output[0]
            alias[n.output[0]] = inp
        elif n.op_type == 'Pad':
            p = a['pads']			# N C H W begins, N C H W ends
            assert a.get('mode', b'constant') == b'constant' and a.get('value', 0.0) == 0.0
            pads[n.output[0]] = (p[2], p[6], p[3], p[7])
            alias[n.output[0]] = ins[0]
        elif n.op_type == 'Conv':
            W = init[n.input[1]].astype(np.float64)
            b = init[n.input[2]].astype(np.float64) if len(n.input) > 2 else np.zeros(W.shape[0])
            k = int(W.shape[2])
            s = a.get('strides', [1, 1])[0]
            cp = a.get('pads', [0, 0, 0, 0])		# top, left, bottom, right
            ep = pads.get(n.input[0], (0, 0, 0, 0))
            pad = (cp[0] + ep[0], cp[2] + ep[1], cp[1] + ep[2], cp[3] + ep[3])
            group = a.get('group', 1)
            dw = group > 1
            assert not dw or group == W.shape[0] and W.shape[1] == 1, n.name
            o = n.output[0]
            bn = only_user(o, 'BatchNormalization')
            if bn is not None:
                gamma, beta, mean, var = (init[x].astype(np.float64) for x in bn.input[1:5])
                f = gamma / np.sqrt(var + attrs(bn).get('epsilon', 1e-5))
                W = W * f[:, None, None, None]
                b = (b - mean) * f + beta
                fused.add(id(bn))
                o = bn.output[0]
            act = 'none'
            for kind in ('Clip', 'Relu'):
                u = only_user(o, kind)
                if u is not None:
                    if kind == 'Clip':
                        ca = attrs(u)
                        assert ca.get('min', 0.0) == 0.0 and ca.get('max', 6.0) == 6.0, u.name
                    act = 'relu6' if kind == 'Clip' else 'relu'
                    fused.add(id(u))
                    o = u.output[0]
            ops.append(dict(op='conv', **{'in': ins[0]}, out=o, k=k, stride=s, pad=pad,
                            ci=int(W.shape[0] if dw else W.shape[1]), co=int(W.shape[0]), dw=dw, act=act))
            weights[o + '.w'] = W.transpose(0, 2, 3, 1).astype(np.float32)
            weights[o + '.b'] = b.astype(np.float32)
            out = o
        elif n.op_type == 'Relu':
            ops.append(dict(op='relu', **{'in': ins[0]}, out=n.output[0]))
        elif n.op_type == 'Add':
            ops.append(dict(op='add', ins=ins, out=n.output[0]))
        elif n.op_type == 'Resize':
            sc = init[n.input[1]].ravel().tolist()
            assert a.get('mode') == b'linear' and sc == [1.0, 1.0, 2.0, 2.0], n.name
            ops.append(dict(op='resize', **{'in': ins[0]}, out=n.output[0]))
        elif n.op_type == 'Squeeze':
            alias[n.output[0]] = ins[0]
        else:
            raise ValueError(f'{n.op_type} {n.name}')
    return inp, ops, weights, out


def conv(x, w, b, k, stride, pad, dw):
    """x [H][W][ci] (float or int as float64), w [co][k][k][ci or 1]"""
    t, bt, l, r = pad
    xp = np.pad(x, ((t, bt), (l, r), (0, 0)))
    Ho = (xp.shape[0] - k) // stride + 1
    Wo = (xp.shape[1] - k) // stride + 1
    if k == 1 and not dw:
        y = xp[:(Ho - 1) * stride + 1:stride, :(Wo - 1) * stride + 1:stride] @ w[:, 0, 0, :].T
        return y + b
    y = np.zeros((Ho, Wo, w.shape[0]), np.float64 if x.dtype == np.float64 else np.float32)
    for ky in range(k):
        for kx in range(k):
            win = xp[ky:ky + (Ho - 1) * stride + 1:stride, kx:kx + (Wo - 1) * stride + 1:stride]
            if dw:
                y += win * w[:, ky, kx, 0]
            else:
                y += (win.reshape(-1, win.shape[2]) @ w[:, ky, kx, :].T.astype(y.dtype)).reshape(Ho, Wo, -1)
    return y + b


def resize2(x):
    """2x bilinear, asymmetric (ONNX opset 10 Resize linear): source
    position = destination / 2, the last row / column repeated"""
    def axis(n):
        src = np.arange(2 * n) / 2
        i0 = np.floor(src).astype(int)
        i1 = np.minimum(i0 + 1, n - 1)
        return i0, i1, (src - i0).astype(np.float32)
    y0, y1, fy = axis(x.shape[0])
    x0, x1, fx = axis(x.shape[1])
    r = x[y0] * (1 - fy)[:, None, None] + x[y1] * fy[:, None, None]
    return r[:, x0] * (1 - fx)[None, :, None] + r[:, x1] * fx[None, :, None]


def act(y, kind):
    if kind == 'relu':
        return np.maximum(y, 0)
    if kind == 'relu6':
        return np.clip(y, 0, 6)
    return y


def last_uses(ops):
    last = {}
    for i, op in enumerate(ops):
        for name in op.get('ins', [op.get('in')]):
            last[name] = i
    return last


def forward(ops, weights, x, on_tensor=None, keep=False):
    """float reference: x normalised [256][256][3] -> {name: tensor}; for
    depthwise convs on_tensor also sees '<out>.nobias' (the conv without
    its bias, which the MDLA computes)"""
    t = {ops[0]['in']: x.astype(np.float32)}
    last = None if keep else last_uses(ops)
    for i, op in enumerate(ops):
        k, o = op['op'], op['out']
        if k == 'conv':
            y = conv(t[op['in']], weights[o + '.w'], 0, op['k'], op['stride'], op['pad'], op['dw'])
            if on_tensor and op['dw']:
                on_tensor(o + '.nobias', y)
            t[o] = act(y + weights[o + '.b'], op['act']).astype(np.float32)
        elif k == 'relu':
            t[o] = np.maximum(t[op['in']], 0)
        elif k == 'add':
            t[o] = t[op['ins'][0]] + t[op['ins'][1]]
        elif k == 'resize':
            t[o] = resize2(t[op['in']])
        if on_tensor:
            on_tensor(o, t[o])
        if last is not None:
            for name in [n for n in t if last.get(n, -1) <= i and n != o]:
                del t[name]
    return t


def prepare(rgb):
    """uint8 H x W x 3 -> the network's input: 256 x 256 (PIL bicubic, as
    MiDaS' transforms), ImageNet normalisation"""
    from PIL import Image
    x = np.asarray(Image.fromarray(rgb).resize((SIZE, SIZE), Image.BICUBIC), np.float32) / 255
    return normalise(x)


def normalise(x):
    """float RGB 0..1 [256][256][3] -> the network's input"""
    return (x - MEAN) / STD


# -- int16 -----------------------------------------------------------------

def split(ops, weights):
    """3 x 3 convs with more than CI_CHUNK input channels -> chunk convs over
    channel slices (c0 / ci; no bias, no activation) + a combine (sum +
    bias + activation on the CPU)"""
    out, w = [], dict(weights)
    for op in ops:
        if op['op'] != 'conv' or op['dw'] or op['k'] == 1 or op['ci'] <= CI_CHUNK:
            out.append(op)
            continue
        o = op['out']
        W, b = w.pop(o + '.w'), w.pop(o + '.b')
        parts = []
        for k, c0 in enumerate(range(0, op['ci'], CI_CHUNK)):
            n = min(CI_CHUNK, op['ci'] - c0)
            name = f'{o}#p{k}'
            out.append(dict(op, out=name, c0=c0, ci=n, act='none'))
            w[name + '.w'] = np.ascontiguousarray(W[..., c0:c0 + n])
            w[name + '.b'] = np.zeros_like(b)
            parts.append(name)
        out.append(dict(op='combine', ins=parts, out=o, act=op['act']))
        w[o + '.bias'] = b
    return out, w


def forward_split(ops, weights, x, on_tensor=None):
    """forward() of a split op list (chunk convs, combines)"""
    t = {ops[0]['in']: x.astype(np.float32)}
    last = last_uses(ops)
    for i, op in enumerate(ops):
        k, o = op['op'], op['out']
        if k == 'conv':
            xin = t[op['in']]
            if 'c0' in op:
                xin = xin[..., op['c0']:op['c0'] + op['ci']]
            y = conv(xin, weights[o + '.w'], 0, op['k'], op['stride'], op['pad'], op['dw'])
            if on_tensor and op['dw']:
                on_tensor(o + '.nobias', y)
            t[o] = act(y + weights[o + '.b'], op['act']).astype(np.float32)
        elif k == 'combine':
            t[o] = act(sum(t[p] for p in op['ins']) + weights[o + '.bias'], op['act']).astype(np.float32)
        elif k == 'relu':
            t[o] = np.maximum(t[op['in']], 0)
        elif k == 'add':
            t[o] = t[op['ins'][0]] + t[op['ins'][1]]
        elif k == 'resize':
            t[o] = resize2(t[op['in']])
        if on_tensor:
            on_tensor(o, t[o])
        for name in [n for n in t if last.get(n, -1) <= i and n != o]:
            del t[name]
    return t


def calibrate(ops, weights, xs):
    rng = {}

    def seen(k, v):
        rng[k] = max(rng.get(k, 0.0), float(np.abs(v).max()))
    for x in xs:
        seen(ops[0]['in'], x)
        forward_split(ops, weights, x, seen)
    return rng


def out_scale(op, r):
    """an output's scale: relu6 at most 6 at the int16 top (the clamp)"""
    v = RANGE * r
    return (min(v, 6.0) if op.get('act') == 'relu6' else v) / A


class Conv:
    """integer parameters of one conv: wq [co][k][k][ci], bq (depthwise:
    none, the CPU adds b in dwfix), mult / sh; y = acc * mult / 2^(31 + sh)"""
    def __init__(self, op, s_in, w, b, s_y, r_acc):
        self.op = op
        w_eff = w.astype(np.float64) * (s_in[:, None, None, None] if op['dw'] else s_in[None, None, None, :])
        self.s_y = s_y
        s_w = max(np.abs(w_eff).max() / A, r_acc * MARGIN / 2 ** 31)
        self.s_w = s_w
        self.wq = rnd(w_eff / s_w).astype(np.int64)
        self.bq = np.zeros(op['co'], np.int64) if op['dw'] else rnd(b / s_w).astype(np.int64)
        f = s_w / s_y
        self.sh = max(0, min(15, int(np.floor(-np.log2(f)))))
        self.mult = int(rnd(f * 2.0 ** (31 + self.sh)))
        assert self.mult < 1 << 31 and np.abs(self.bq).max(initial=0) < 2 ** 31, op['out']

    def run(self, x):
        op = self.op
        acc = conv(x.astype(np.float64), self.wq.astype(np.float64), 0, op['k'], op['stride'], op['pad'], op['dw'])
        acc = np.clip(acc.astype(np.int64) + self.bq, -2 ** 31, 2 ** 31 - 1)
        S = 31 + self.sh
        y = np.clip((acc * self.mult + (1 << (S - 1))) >> S, -A - 1, A)
        return np.maximum(y, 0) if op['act'] != 'none' and not op['dw'] else y


def q16(r):
    """float32 (overwritten) -> int16 range, floor(v + 0.5) (seg16.q16)"""
    r += np.float32(0.5)
    np.floor(r, out=r)
    np.clip(r, -A - 1, A, out=r)
    return r.astype(np.int32)


def cpu_dwfix(y, s0, b, s_y, kind):
    """a depthwise conv's MDLA output (no bias) -> + bias, activation, at
    the output's scale"""
    r = y.astype(np.float32) * np.float32(s0 / s_y) + (b / s_y).astype(np.float32)
    if kind != 'none':
        np.maximum(r, 0, out=r)		# relu6: the top is the scale's (6 at most)
    return q16(r)


def cpu_add(a, sa, b, sb, sy):
    r = a.astype(np.float32) * np.float32(sa / sy)
    r += b.astype(np.float32) * np.float32(sb / sy)
    return q16(r)


def cpu_resize(x):
    return q16(resize2(x.astype(np.float32)))


def cpu_combine(parts, scales, bias, s_out, kind):
    acc = (bias / s_out).astype(np.float32)
    for p, sc in zip(parts, scales):
        acc = acc + p.astype(np.float32) * np.float32(sc / s_out)
    y = q16(acc)
    return np.maximum(y, 0) if kind != 'none' else y


def quantise(ops, weights, rng):
    """-> (convs {out: Conv}, scales {tensor: float}); depthwise convs also
    '<out>.nobias' (the MDLA output's scale)"""
    first = ops[0]['in']
    s = {first: RANGE * rng[first] / A}
    convs = {}
    for op in ops:
        k, o = op['op'], op['out']
        if k == 'conv':
            s[o] = out_scale(op, rng[o])
            s_in = np.full(op['ci'], s[op['in']])
            if op['dw']:
                s[o + '.nobias'] = RANGE * rng[o + '.nobias'] / A
                convs[o] = Conv(op, s_in, weights[o + '.w'], weights[o + '.b'], s[o + '.nobias'],
                                RANGE * rng[o + '.nobias'])
            else:
                convs[o] = Conv(op, s_in, weights[o + '.w'], weights[o + '.b'], s[o], RANGE * rng[o])
        elif k == 'combine':
            s[o] = out_scale(op, rng[o])
        elif k in ('relu', 'resize'):
            s[o] = s[op['in']]
        elif k == 'add':
            s[o] = RANGE * rng[o] / A
    return convs, s


def qforward(ops, convs, s, weights, x, on_tensor=None):
    """the integer network on a normalised float input -> depth (float)"""
    first = ops[0]['in']
    t = {first: np.clip(rnd(x / s[first]), -A - 1, A).astype(np.int64)}
    last = last_uses(ops)
    for i, op in enumerate(ops):
        k, o = op['op'], op['out']
        if k == 'conv':
            xin = t[op['in']]
            if 'c0' in op:
                xin = xin[..., op['c0']:op['c0'] + op['ci']]
            y = convs[o].run(xin)
            if op['dw']:
                if on_tensor:
                    on_tensor(o + '.nobias', y)
                y = cpu_dwfix(y, s[o + '.nobias'], weights[o + '.b'], s[o], op['act'])
            t[o] = y
        elif k == 'combine':
            t[o] = cpu_combine([t[p] for p in op['ins']], [s[p] for p in op['ins']], weights[o + '.bias'],
                               s[o], op['act'])
        elif k == 'relu':
            t[o] = np.maximum(t[op['in']], 0)
        elif k == 'add':
            a, b = op['ins']
            t[o] = cpu_add(t[a], s[a], t[b], s[b], s[o])
        elif k == 'resize':
            t[o] = cpu_resize(t[op['in']])
        if on_tensor:
            on_tensor(o, t[o])
        for name in [n for n in t if last.get(n, -1) <= i and n != o]:
            del t[name]
    out = ops[-1]['out']
    return t[out][..., 0] * s[out]


def load(path):
    """the split op list and float weights from the ONNX model"""
    inp, ops, weights, out = convert(path)
    return split(ops, weights)


def export(ops, convs, s, weights, dst):
    """the runner's model: ops, int16 weights, requant, the CPU steps'
    float biases, every tensor's scale"""
    out = {'meta': np.frombuffer(json.dumps(dict(ops=ops, input=ops[0]['in'], output=ops[-1]['out'])).encode(),
                                 np.uint8)}
    for o, c in convs.items():
        out[o + '.wq'] = c.wq.astype(np.int16)
        out[o + '.bq'] = c.bq.astype(np.int32)
        out[o + '.rq'] = np.array([c.mult, c.sh], np.int64)
        if c.op['dw']:
            out[o + '.bias'] = weights[o + '.b'].astype(np.float64)
    for op in ops:
        if op['op'] == 'combine':
            out[op['out'] + '.bias'] = weights[op['out'] + '.bias'].astype(np.float64)
    for name, sc in s.items():
        out[name + '.s'] = np.array([sc], np.float64)
    np.savez(dst, **out)


def pictures(paths):
    """normalised inputs, each also mirrored"""
    from PIL import Image
    xs = []
    for p in paths:
        im = Image.open(p)
        from PIL import ImageOps
        rgb = np.asarray(ImageOps.exif_transpose(im).convert('RGB'))
        xs += [prepare(rgb), prepare(rgb[:, ::-1].copy())]
    return xs


def check(path, photos):
    import onnxruntime as ort
    inp, ops, weights, out = convert(path)
    print(len(ops), 'ops:', {k: sum(o['op'] == k for o in ops) for k in ('conv', 'relu', 'add', 'resize')},
          'depthwise', sum(o['op'] == 'conv' and o['dw'] for o in ops))
    sess = ort.InferenceSession(path, providers=['CPUExecutionProvider'])
    xs = [np.random.default_rng(0).random((SIZE, SIZE, 3)).astype(np.float32)]
    if photos:
        from PIL import Image
        xs += [np.asarray(Image.open(p).convert('RGB').resize((SIZE, SIZE), Image.BICUBIC), np.float32) / 255
               for p in photos]
    for x in xs:
        ref = sess.run(None, {sess.get_inputs()[0].name: x.transpose(2, 0, 1)[None]})[0][0]
        y = forward(ops, weights, normalise(x))[out][..., 0]
        print(f'max |numpy - onnxruntime| {np.abs(y - ref).max():.2e} (output range {ref.max():.1f})')


def main():
    cmd, path, *rest = sys.argv[1:]
    if cmd == 'check':
        check(path, rest)
        return
    ops, weights = load(path)
    if cmd == 'calibrate':
        rng = calibrate(ops, weights, pictures(rest[1:]))
        with open(rest[0], 'w') as f:
            json.dump({k: round(v, 6) for k, v in rng.items()}, f, indent=0)
        print(len(rng), 'ranges ->', rest[0])
        return
    with open(rest[0]) as f:
        rng = json.load(f)
    convs, s = quantise(ops, weights, rng)
    if cmd == 'export':
        export(ops, convs, s, weights, rest[1])
        return
    out = ops[-1]['out']
    for i, x in enumerate(pictures(rest[1:])[::2]):
        ref = forward_split(ops, weights, x)[out][..., 0]
        q = qforward(ops, convs, s, weights, x)
        d = np.abs(q - ref) / max(ref.max(), 1e-6)
        corr = np.corrcoef(q.ravel(), ref.ravel())[0, 1]
        print(f'picture {i}: max |int16 - float| {d.max():.4f}, mean {d.mean():.5f} of the range, '
              f'correlation {corr:.6f}', flush=True)


if __name__ == '__main__':
    main()
