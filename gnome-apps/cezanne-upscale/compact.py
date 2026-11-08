"""The compact Real-ESRGAN (SRVGGNetCompact: Upscayl's upscayl-lite-4x =
realesr-general-x4v3, the camera app's GPU fallback) for the MDLA: ncnn
loader, float reference, int16 quantisation and its exact integer
simulation (x4plus's conventions, quant16.py).

    net = load('/usr/share/realesrgan/models/upscayl-lite-4x', scale=2)
    y = forward(net, x)          # x float32 HWC RGB in [0, 1] -> scale x

Structure: 34 convs 3x3 pad 1 (3 -> 64, 32 x 64 -> 64, 64 -> 48), a
per-channel PReLU after each but the last, PixelShuffle 4, plus the input
nearest 4x. scale=2: the 4x result averaged over 2 x 2 blocks, which is
linear: the last conv's 48 outputs averaged in groups of 4 (the pixels of
each 2 x 2 block) -> 12 outputs, PixelShuffle 2, plus the input nearest 2x.

    compact.py calibrate <model base> ranges.json picture...   (dev)
    compact.py export <model base> ranges.json out.npz          (the package)
    compact.py compare <model base> ranges.json picture...      (dev)
"""
import json
import struct
import sys

import numpy as np

FP16_TAG = 0x01306b47
A = 32767
MARGIN = 4.0
RANGE = 1.25
N_CONV = 34


def load(base, scale=4):
    """-> dict(convs=[(w [co][3][3][ci], b)], slopes=[per-channel PReLU], scale)"""
    lines = open(base + '.param').read().split('\n')[2:]
    blob = open(base + '.bin', 'rb').read()
    off, convs, slopes = 0, [], []
    for line in lines:
        t = line.split()
        if not t:
            continue
        p = dict(kv.split('=', 1) for kv in t[6:] if '=' in kv)
        if t[0] == 'Convolution':
            co, k, ws = int(p['0']), int(p['1']), int(p['6'])
            assert k == 3 and struct.unpack_from('<I', blob, off)[0] == FP16_TAG
            w = np.frombuffer(blob, np.float16, ws, off + 4).astype(np.float32)
            off += 4 + ws * 2 + (2 if ws % 2 else 0)
            b = np.frombuffer(blob, np.float32, co, off).copy()
            off += co * 4
            convs.append((w.reshape(co, ws // (co * 9), 3, 3).transpose(0, 2, 3, 1).copy(), b))
        elif t[0] == 'PReLU':
            n = int(p['0'])
            slopes.append(np.frombuffer(blob, np.float32, n, off).copy())
            off += n * 4
        elif t[0] == 'PixelShuffle':
            assert p['0'] == '4'
    assert off == len(blob) and len(convs) == N_CONV and len(slopes) == N_CONV - 1, (off, len(blob))
    if scale == 2:			# 2 x 2 block averages of the 4x output
        w, b = convs[-1]
        # PixelShuffle 4: channel c * 16 + i * 4 + j -> pixel (4y + i, 4x + j);
        # i = 2a + p, j = 2b + q: averaged over p, q -> channel c * 4 + a * 2 + b
        w = w.reshape(3, 2, 2, 2, 2, 3, 3, 64).mean(axis=(2, 4)).reshape(12, 3, 3, 64)
        b = b.reshape(3, 2, 2, 2, 2).mean(axis=(2, 4)).reshape(12)
        convs[-1] = (w.astype(np.float32), b.astype(np.float32))
    else:
        assert scale == 4
    return dict(convs=convs, slopes=slopes, scale=scale)


def conv3(x, w, b):
    H, W, ci = x.shape
    xp = np.zeros((H + 2, W + 2, ci), np.float64)
    xp[1:-1, 1:-1] = x
    acc = np.zeros((H * W, w.shape[0]))
    for ky in range(3):
        for kx in range(3):
            acc += xp[ky:ky + H, kx:kx + W].reshape(-1, ci) @ w[:, ky, kx, :].T.astype(np.float64)
    return acc.reshape(H, W, -1) + b


def shuffle(y, r):
    """[H][W][3 r r] -> [rH][rW][3] (PixelShuffle: channel c r r + i r + j)"""
    H, W = y.shape[:2]
    return y.reshape(H, W, 3, r, r).transpose(0, 3, 1, 4, 2).reshape(H * r, W * r, 3)


def finish(last, x, r):
    """the last conv's output (float) -> the picture: shuffled + nearest input"""
    return np.clip(shuffle(last, r) + x.repeat(r, 0).repeat(r, 1), 0, 1)


def forward(net, x, hook=None):
    """x float HWC RGB [0, 1] -> scale x (float); hook(i, pre-activation)"""
    h = x
    for i, (w, b) in enumerate(net['convs']):
        y = conv3(h, w, b)
        if hook:
            hook(i, y)
        if i < N_CONV - 1:
            s = net['slopes'][i]
            y = np.where(y >= 0, y, y * s)
        h = y
    return finish(h, x, net['scale'])


def calibrate(net, xs):
    rng = [0.0] * N_CONV

    def hook(i, y):
        rng[i] = max(rng[i], float(np.abs(y).max()))
    for x in xs:
        forward(net, x, hook)
    return rng


def rnd(v):
    return np.floor(v + 0.5)


def q31(f, base):
    """f -> (m, s): f = m / 2^(base + s) (quant16.q31); 0 -> (0, 0)"""
    if f == 0:
        return 0, 0
    e = int(np.floor(np.log2(abs(f))))
    s = 30 - base - e
    m = int(rnd(abs(f) * 2.0 ** (base + s)))
    if m >= 1 << 31:
        m >>= 1
        s -= 1
    return (m if f > 0 else -m), s


class Layer:
    """integer parameters of one conv: wq [co][3][3][ci], bq, mult / sh;
    PReLU: positive factor 1 (pm, ps), per-channel negative (nm, ns arrays)"""
    def __init__(self, s_in, w, b, slope, rng):
        w_eff = w.astype(np.float64) * s_in
        self.s_y = RANGE * rng / A
        s_w = max(np.abs(w_eff).max() / A, RANGE * rng * MARGIN / 2 ** 31)
        self.wq = rnd(w_eff / s_w).astype(np.int64)
        self.bq = rnd(b / s_w).astype(np.int64)
        f = s_w / self.s_y
        self.sh = max(0, min(15, int(np.floor(-np.log2(f)))))
        self.mult = int(rnd(f * 2.0 ** (31 + self.sh)))
        assert self.mult < 1 << 31 and np.abs(self.bq).max() < 2 ** 31
        self.prelu = None
        if slope is not None:
            pm, ps = q31(1.0, 19)
            ns = [q31(float(v), 19) for v in slope]
            # the table's shift is a uint8: a slope's exponent stays in range
            assert all(0 <= s < 256 for _, s in ns), 'PReLU shift out of range'
            self.prelu = (pm, ps, np.array([m for m, _ in ns], np.int64),
                          np.array([s for _, s in ns], np.int64))

    def run(self, x):
        """x int [H][W][ci] -> int16-range output, the hardware arithmetic"""
        acc = conv3(x.astype(np.float64), self.wq.astype(np.float64), 0)
        acc = np.clip(acc.astype(np.int64) + self.bq, -2 ** 31, 2 ** 31 - 1)
        S = 31 + self.sh
        y = np.clip((acc * self.mult + (1 << (S - 1))) >> S, -A - 1, A)
        if self.prelu:
            pm, ps, nm, ns = self.prelu
            m = np.where(y >= 0, pm, nm)
            s = 19 + np.where(y >= 0, ps, ns)
            y = np.clip((y * m + (np.int64(1) << (s - 1))) >> s, -A - 1, A)
        return y


def quantise(net, rng):
    """-> (layers, input scale): the input in [0, 1] -> int16 at 1 / 32767"""
    s_x = 1.0 / A
    s = np.full(3, s_x)
    layers = []
    for i, (w, b) in enumerate(net['convs']):
        slope = net['slopes'][i] if i < N_CONV - 1 else None
        L = Layer(s, w, b, slope, rng[i])
        layers.append(L)
        s = np.full(w.shape[0], L.s_y)
    return layers, s_x


def qforward(layers, s_x, x, scale):
    """the integer network on a float input -> scale x (float)"""
    h = np.clip(rnd(x / s_x), 0, A).astype(np.int64)
    for L in layers:
        h = L.run(h)
    return finish(h * layers[-1].s_y, x, scale)


def export(layers, s_x, scale, dst):
    out = dict(scale=np.array(scale), s_x=np.array(s_x), s_last=np.array(layers[-1].s_y))
    for i, L in enumerate(layers):
        out[f'w{i}'] = L.wq.astype(np.int16)
        out[f'b{i}'] = L.bq.astype(np.int32)
        out[f'rq{i}'] = np.array([L.mult, L.sh], np.int64)
        if L.prelu:
            pm, ps, nm, ns = L.prelu
            out[f'pp{i}'] = np.array([pm, ps], np.int64)
            out[f'pn{i}'] = nm.astype(np.int32)
            out[f'ps{i}'] = ns.astype(np.uint8)
    np.savez(dst, **out)


def crops(paths, size=160, per=3, seed=0):
    """float crops of pictures (calibration / comparison)"""
    from PIL import Image
    rng = np.random.default_rng(seed)
    out = []
    for p in paths:
        im = np.asarray(Image.open(p).convert('RGB'), np.float32) / 255
        for _ in range(per):
            y = rng.integers(0, im.shape[0] - size)
            x = rng.integers(0, im.shape[1] - size)
            out.append(im[y:y + size, x:x + size])
    return out


def main():
    cmd, base, ranges, *rest = sys.argv[1:]
    if cmd == 'export':
        with open(ranges) as f:
            rng = json.load(f)
        for scale in (2, 4):
            net = load(base, scale)
            layers, s_x = quantise(net, rng[str(scale)])
            export(layers, s_x, scale, rest[0].replace('.npz', f'-x{scale}.npz'))
        return
    if cmd == 'calibrate':
        xs = crops(rest)
        rng = {str(scale): [round(v, 6) for v in calibrate(load(base, scale), xs)] for scale in (2, 4)}
        with open(ranges, 'w') as f:
            json.dump(rng, f)
        print('ranges ->', ranges)
        return
    with open(ranges) as f:
        rng = json.load(f)
    xs = crops(rest, per=1, seed=7)
    for scale in (2, 4):
        net = load(base, scale)
        layers, s_x = quantise(net, rng[str(scale)])
        for i, x in enumerate(xs[:4]):
            f = forward(net, x)
            q = qforward(layers, s_x, x, scale)
            mse = ((f - q) ** 2).mean()
            print(f'x{scale} crop {i}: int16 vs float {10 * np.log10(1 / max(mse, 1e-20)):.1f} dB, '
                  f'max |diff| {np.abs(f - q).max() * 255:.2f} levels')


if __name__ == '__main__':
    main()
