"""Real-ESRGAN x4plus (RRDBNet; Upscayl's upscayl-standard-4x is the same
file) in numpy: ncnn model loader and the float reference forward pass.

    net = load('build/upscaler/models/upscayl-standard-4x')
    y = forward(net, x)            # x float32 HWC RGB in [0, 1], y 4x

Structure (ncnn graph, 351 convs, all 3x3 pad 1):
  feat = conv_first(x)                                   3 -> 64
  23 RRDB: 3 RDB each: c1..c4 = lrelu(conv(cat(x, c1..)))  64/96/128/160 -> 32
           c5 = conv(cat(x, c1..c4))                       192 -> 64
           rdb = 0.2 * c5 + x;   rrdb = 0.2 * rdb3 + x_in
  body = conv_body(trunk) + feat                         64 -> 64
  up1 = lrelu(conv(nearest2x(body))), up2 = lrelu(conv(nearest2x(up1)))
  out = conv_last(lrelu(conv_hr(up2)))                   64 -> 3
"""
import struct
import numpy as np

FP16_TAG = 0x01306b47


def load(base):
    """-> list of (weight [co][ky][kx][ci] float32, bias float32, leaky slope or
    None) in graph order"""
    lines = open(base + '.param').read().split('\n')[2:]
    blob = open(base + '.bin', 'rb').read()
    off, convs = 0, []
    for l in lines:
        t = l.split()
        if not t or t[0] != 'Convolution':
            continue
        p = dict(kv.split('=', 1) for kv in t[6:] if '=' in kv)
        co, k, ws = int(p['0']), int(p['1']), int(p['6'])
        assert struct.unpack_from('<I', blob, off)[0] == FP16_TAG
        w = np.frombuffer(blob, np.float16, ws, off + 4).astype(np.float32)
        off += 4 + ws * 2 + (2 if ws % 2 else 0)
        b = np.frombuffer(blob, np.float32, co, off).copy()
        off += co * 4
        ci = ws // (co * k * k)
        w = w.reshape(co, ci, k, k).transpose(0, 2, 3, 1).copy()
        slope = float(p['-23310'].split(',')[1]) if p.get('9') == '2' else None
        convs.append((w, b, slope))
    assert off == len(blob) and len(convs) == 351
    return convs


def conv3(x, w, b):
    """3x3 pad 1 conv, x [H][W][ci] float32 -> [H][W][co]"""
    H, W, ci = x.shape
    co = w.shape[0]
    xp = np.zeros((H + 2, W + 2, ci), np.float32)
    xp[1:-1, 1:-1] = x
    acc = np.broadcast_to(b, (H * W, co)).copy()
    for ky in range(3):
        for kx in range(3):
            acc += xp[ky:ky + H, kx:kx + W].reshape(-1, ci) @ w[:, ky, kx, :].T
    return acc.reshape(H, W, co)


def lrelu(x, s):
    return np.where(x >= 0, x, x * s)


def up2(x):
    return x.repeat(2, axis=0).repeat(2, axis=1)


def forward(net, x, conv=conv3, hook=None):
    """float forward; hook(name, tensor) sees every intermediate tensor"""
    hook = hook or (lambda n, t: None)
    it = iter(net)

    def layer(name, inp):
        w, b, s = next(it)
        y = conv(inp, w, b)
        hook(name + '.pre', y)
        if s is not None:
            y = lrelu(y, s)
        hook(name, y)
        return y

    feat = layer('first', x)
    t = feat
    for r in range(23):
        t0 = t
        for d in range(3):
            cat = [t]
            for k in range(4):
                cat.append(layer(f'r{r}.{d}.c{k + 1}', np.concatenate(cat, axis=2)))
            c5 = layer(f'r{r}.{d}.c5', np.concatenate(cat, axis=2))
            t = 0.2 * c5 + t
            hook(f'r{r}.{d}', t)
        t = 0.2 * t + t0
        hook(f'r{r}', t)
    body = layer('body', t) + feat
    hook('body+', body)
    u = layer('up1', up2(body))
    u = layer('up2', up2(u))
    u = layer('hr', u)
    return layer('last', u)
