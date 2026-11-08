"""int16 quantisation of Real-ESRGAN x4plus as the MDLA's int16 mode runs it
(mainline/apu/README.md "int16"), with exact integer arithmetic:

- activations int16 symmetric (zero point 0), scale = calibrated |max| /
  32767, per concat slice; padding 0
- weights int16 symmetric per tensor with the input slices' scales folded
  in; the weight scale is also bounded so that the largest accumulator seen
  in calibration * MARGIN stays below 2^31 (the int16 accumulator saturates
  at 32 bits: re/e16_acc16.py)
- requant to int16 with the Q31 multiplier, LeakyReLU in the PReLU stage at
  the same scale (positive factor 1, negative 0.2)
- residual adds as 1x1 convs with int16 weights; rounding half up

    params = quantise(net, cal)          # per layer integer parameters
    y8 = qforward16(net, x8, cal)        # simulation (uses quantise())
"""
import numpy as np
import esrgan

A = 32767
MARGIN = 4.0				# accumulator headroom below 2^31
RANGE = 2.0				# activation headroom over the calibrated ranges
OUT_ONE = 32640				# the last layer's 1.0 (= 255 * 128)
STATS = {}				# name -> (weight code max, max |acc| / 2^31) of the last run


def rnd(v):
    return np.floor(v + 0.5)


def amax(rng):
    return RANGE * max(abs(rng[0]), abs(rng[1]), 1e-12)


def q31(f, base):
    """f -> (m, s): f = m / 2^(base + s), |m| in [2^30, 2^31) (PReLU stage:
    base 19 in int16; conv requant: base 31, s = 0..15)"""
    e = int(np.floor(np.log2(abs(f))))
    s = 30 - base - e
    m = int(rnd(abs(f) * 2.0 ** (base + s)))
    if m >= 1 << 31:
        m >>= 1; s -= 1
    return (m if f > 0 else -m), s


class Layer:
    """integer parameters of one conv / add: wq [co][k][k][ci] int, bias int,
    mult / sh (y = acc * mult / 2^(31 + sh)), PReLU (pm, ps, nm, ns) or None"""
    def __init__(self, name, wq, bq, f, slope, s_y):
        self.name, self.wq, self.bq, self.s_y = name, wq.astype(np.int64), bq.astype(np.int64), s_y
        self.sh = max(0, min(15, int(np.floor(-np.log2(f)))))
        self.mult = int(rnd(f * 2.0 ** (31 + self.sh)))
        assert self.mult < 1 << 31 and np.abs(self.bq).max() < 2 ** 31
        self.prelu = None
        if slope is not None:
            pm, ps = q31(1.0, 19)
            nm, ns = q31(slope, 19)
            self.prelu = (pm, ps, nm, ns)

    def run(self, x):
        """x int64 [H][W][ci] -> int64 output, the hardware arithmetic"""
        w = self.wq.astype(np.float64)
        if w.shape[1] == 1:
            acc = x.astype(np.float64) @ w[:, 0, 0, :].T
        else:
            H, W, ci = x.shape
            xp = np.zeros((H + 2, W + 2, ci))
            xp[1:-1, 1:-1] = x
            acc = np.zeros((H * W, w.shape[0]))
            for ky in range(3):
                for kx in range(3):
                    acc += xp[ky:ky + H, kx:kx + W].reshape(-1, ci) @ w[:, ky, kx, :].T
            acc = acc.reshape(H, W, -1)
        acc = acc.astype(np.int64) + self.bq
        STATS[self.name] = (int(np.abs(self.wq).max()), float(np.abs(acc).max() / 2 ** 31))
        acc = np.clip(acc, -2 ** 31, 2 ** 31 - 1)
        S = 31 + self.sh
        y = np.clip((acc * self.mult + (1 << (S - 1))) >> S, -A - 1, A)
        if self.prelu:
            pm, ps, nm, ns = self.prelu
            m = np.where(y >= 0, pm, nm)
            s = 19 + np.where(y >= 0, ps, ns)
            y = np.clip((y * m + (1 << (s - 1))) >> s, -A - 1, A)
        return y


def conv_layer(name, s_in, w, b, slope, y_rng):
    w_eff = w.astype(np.float64) * s_in
    s_y = amax(y_rng) / A
    s_w = max(np.abs(w_eff).max() / A, amax(y_rng) * MARGIN / 2 ** 31)
    return Layer(name, rnd(w_eff / s_w), rnd(b / s_w), s_w / s_y, slope, s_y)


def add_layer(name, terms, y_rng):
    """terms: (scale, coefficient) per 64-channel operand -> 1x1 conv over
    the concatenated operands with diagonal weights"""
    s_y = amax(y_rng) / A
    w_eff = np.array([c * s for s, c in terms])
    s_w = max(np.abs(w_eff).max() / A, amax(y_rng) * MARGIN / 2 ** 31)
    n = len(terms)
    wq = np.zeros((64, 1, 1, 64 * n))
    for i, (s, c) in enumerate(terms):
        wq[np.arange(64), 0, 0, 64 * i + np.arange(64)] = rnd(c * s / s_w)
    return Layer(name, wq, np.zeros(64), s_w / s_y, None, s_y)


def quantise(net, cal):
    """-> dict name -> Layer, in network order (dict order). Adds take their
    operands as [x0 | c5 | x] (RRDB end) or [c5 | x] / [body | feat] (the
    runner's buffer layout)"""
    L = {}
    it = iter(net)

    def conv(name, s_in):
        w, b, slope = next(it)
        rng = cal[name + '.pre']
        if slope is not None:			# one scale before and after LeakyReLU
            rng = (min(rng[0], 0), rng[1])
        L[name] = conv_layer(name, s_in, w, b, slope, rng)
        return np.full(w.shape[0], L[name].s_y)

    s_x = conv('first', np.full(3, 1 / 255))
    s_feat = s_x
    for r in range(23):
        s_x0 = s_x
        for d in range(3):
            s_cat = s_x
            for k in range(4):
                s_cat = np.concatenate([s_cat, conv(f'r{r}.{d}.c{k + 1}', s_cat)])
            s5 = conv(f'r{r}.{d}.c5', s_cat)
            if d < 2:
                L[f'r{r}.{d}.add'] = add_layer(f'r{r}.{d}.add', [(s5[0], 0.2), (s_x[0], 1.0)], cal[f'r{r}.{d}'])
            else:
                L[f'r{r}.add'] = add_layer(f'r{r}.add', [(s_x0[0], 1.0), (s5[0], 0.04), (s_x[0], 0.2)], cal[f'r{r}'])
            s_x = np.full(64, L[f'r{r}.{d}.add' if d < 2 else f'r{r}.add'].s_y)
    s_b = conv('body', s_x)
    L['body.add'] = add_layer('body.add', [(s_b[0], 1.0), (s_feat[0], 1.0)], cal['body+'])
    s = conv('up1', np.full(64, L['body.add'].s_y))
    s = conv('up2', s)
    s = conv('hr', s)
    w, b, _ = next(it)
    L['last'] = conv_layer('last', s, w, b, None, (0.0, A / OUT_ONE / RANGE))	# 1.0 -> OUT_ONE
    return L


def qforward16(net, x8, cal, L=None, hook=None):
    """simulation of the MDLA run: x8 uint8 HWC RGB -> uint8 4x"""
    L = L or quantise(net, cal)
    hook = hook or (lambda n, t: None)
    feat = L['first'].run(x8.astype(np.int64))
    t = feat
    for r in range(23):
        t0 = t
        for d in range(3):
            cat = t
            for k in range(4):
                cat = np.concatenate([cat, L[f'r{r}.{d}.c{k + 1}'].run(cat)], axis=2)
            c5 = L[f'r{r}.{d}.c5'].run(cat)
            if d < 2:
                t = L[f'r{r}.{d}.add'].run(np.concatenate([c5, t], axis=2))
            else:
                t = L[f'r{r}.add'].run(np.concatenate([t0, c5, t], axis=2))
            hook(f'r{r}.{d}', t)
    body = L['body'].run(t)
    body = L['body.add'].run(np.concatenate([body, feat], axis=2))
    u = L['up1'].run(esrgan.up2(body))
    u = L['up2'].run(esrgan.up2(u))
    u = L['hr'].run(u)
    y = L['last'].run(u)
    return np.clip((y + 64) >> 7, 0, 255).astype(np.uint8)
