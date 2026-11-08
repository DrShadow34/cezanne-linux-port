"""uint8 quantisation of Real-ESRGAN x4plus as the MDLA runs it, simulated in
numpy (mainline/apu/README.md: conv, PReLU stage):

- every tensor uint8 with its own scale / zero point (a concatenated RDB
  input is a buffer of slices, each with its own); padding = the slice's
  zero point (bordered buffers, not the conv's single pad value)
- conv: weights uint8 per tensor with the input slices' scales folded in,
  int32 bias, requantised to uint8 (pre-activation range), then the PReLU
  stage: d = y - zp; d * s_y / s_o (d >= 0) or d * 0.2 * s_y / s_o
- residual adds (0.2 * c5 + x, the RRDB's 0.2 * rdb + x, body + feat) as
  1x1 convs over the operand slices, weights = the coefficients
- rounding half up everywhere, as the hardware

    cal = calibrate(net, crops)          # per-tensor min / max
    y8 = qforward(net, x8, cal)          # uint8 in, uint8 out (4x)
"""
import numpy as np
import esrgan

ALEV = 255		# activation levels (65535: 16-bit, for experiments)
WLEV = 255		# weight levels
PREC = None		# experiments: name -> (ALEV, WLEV) per layer
SNAP = False		# experiments: exact residual-add weight ratios


def qparams(lo, hi, lev=None):
    lev = lev or ALEV
    lo, hi = min(lo, 0.0), max(hi, 0.0)
    s = (hi - lo) / lev or 1.0
    return s, int(np.clip(np.floor(-lo / s + 0.5), 0, lev))


def rnd(v):
    return np.floor(v + 0.5)


class QT:
    """a uint8 tensor (as float32 values 0..255) with per-channel scale / zp
    (constant per slice)"""
    def __init__(self, q, s, z):
        self.q = q
        C = q.shape[2]
        self.s = np.broadcast_to(np.asarray(s, np.float32), (C,)).copy()
        self.z = np.broadcast_to(np.asarray(z, np.float32), (C,)).copy()

    @staticmethod
    def cat(ts):
        return QT(np.concatenate([t.q for t in ts], axis=2),
                  np.concatenate([t.s for t in ts]), np.concatenate([t.z for t in ts]))

    def real(self):
        return (self.q - self.z) * self.s


def calibrate(net, crops, pct=None):
    """float forward over the crops; per tensor (min, max), or the pct / 100 - pct
    percentiles"""
    cal = {}

    def hook(n, t):
        lo, hi = (t.min(), t.max()) if pct is None else np.percentile(t, (100 - pct, pct))
        a = cal.get(n, (np.inf, -np.inf))
        cal[n] = (min(a[0], float(lo)), max(a[1], float(hi)))
    for x in crops:
        esrgan.forward(net, x, hook=hook)
    return cal


def snap(rng, coef, s_other):
    """(scale, zp) for a tensor of calibrated range rng that a residual add
    takes with coefficient coef next to a tensor of scale s_other: the
    smallest scale >= the calibrated one at which the two 8-bit add weights
    have the exact ratio (larger one = WLEV)"""
    lo, hi = min(rng[0], 0.0), max(rng[1], 0.0)
    s = qparams(lo, hi)[0]
    r = coef * s / s_other
    if r <= 1:
        s = np.ceil(r * WLEV - 1e-9) * s_other / (coef * WLEV)
    else:
        s = WLEV * s_other / (coef * np.floor(WLEV / r + 1e-9))
    return s, int(np.clip(np.floor(-lo / s + 0.5), 0, ALEV))


def quant_weights(w_eff):
    """per-tensor asymmetric uint8 -> (w_q - zw) as float32, s_w, zw"""
    lo, hi = float(w_eff.min()), float(w_eff.max())
    s, zw = qparams(lo, hi, WLEV)
    wq = np.clip(rnd(w_eff / s) + zw, 0, WLEV)
    return (wq - zw).astype(np.float32), s, zw


BC = {}			# bias correction per layer name (bias_correct())
COLLECT = None		# name -> list of per-channel mean output shifts


def qconv(x, w, b, slope, cal_pre, cal_post, name=None, out_q=None):
    """x QT -> QT: conv 3x3 + requant (+ PReLU stage); out_q overrides the
    pre-activation (scale, zp)"""
    w_eff = w * x.s					# fold the input slice scales
    wd, s_w, zw = quant_weights(w_eff)
    if COLLECT is not None and name:
        err = esrgan.conv3(x.q - x.z, wd * s_w - w_eff, np.zeros(w.shape[0], np.float32))
        COLLECT.setdefault(name, []).append(err.mean(axis=(0, 1)))
    b = b - BC.get(name, 0)
    acc = esrgan.conv3(x.q - x.z, wd, (b / s_w).astype(np.float32)) * s_w
    s_y, z_y = out_q or qparams(*cal_pre)
    y = np.clip(rnd(acc / s_y) + z_y, 0, ALEV)
    if slope is None:
        return QT(y, s_y, z_y)
    s_o, z_o = qparams(*cal_post)
    d = y - z_y
    v = np.where(d >= 0, d * (s_y / s_o), d * (slope * s_y / s_o))
    return QT(np.clip(rnd(v) + z_o, 0, ALEV), s_o, z_o)


def qadd(terms, cal_out, out_q=None):
    """sum(coef * t) as a 1x1 conv: weights coef * s_t, uint8 per tensor (all
    positive: zw = 0)"""
    w_eff = np.array([c * t.s[0] for t, c in terms])
    s_w = w_eff.max() / WLEV
    wq = rnd(w_eff / s_w)
    acc = sum(wq[i] * (t.q - t.z) for i, (t, _) in enumerate(terms)) * s_w
    s_o, z_o = out_q or qparams(*cal_out)
    return QT(np.clip(rnd(acc / s_o) + z_o, 0, ALEV), s_o, z_o)


def qforward(net, x8, cal):
    """x8 uint8 HWC RGB -> uint8 4x output"""
    it = iter(net)

    def layer(name, x, out_q=None):
        global ALEV, WLEV
        w, b, slope = next(it)
        if PREC:
            ALEV, WLEV = PREC(name)
        return qconv(x, w, b, slope, cal[name + '.pre'], cal.get(name), name, out_q)

    feat = layer('first', QT(x8.astype(np.float32), 1 / 255, 0))	# pixels, 8 bit
    t = feat
    for r in range(23):
        t0 = t
        for d in range(3):
            cat = [t]
            for k in range(4):
                cat.append(layer(f'r{r}.{d}.c{k + 1}', QT.cat(cat)))
            if SNAP:	# exact 8-bit add weight ratios (scales snapped; worse in tests)
                c5 = layer(f'r{r}.{d}.c5', QT.cat(cat), snap(cal[f'r{r}.{d}.c5.pre'], 0.2, t.s[0]))
                out_q = snap(cal[f'r{r}.{d}'], 0.2, t0.s[0]) if d == 2 else None
                t = qadd([(c5, 0.2), (t, 1.0)], cal[f'r{r}.{d}'], out_q)
                continue
            c5 = layer(f'r{r}.{d}.c5', QT.cat(cat))
            if d < 2:
                t = qadd([(c5, 0.2), (t, 1.0)], cal[f'r{r}.{d}'])
            else:					# rdb3 and the RRDB residual in one
                t = qadd([(c5, 0.04), (t, 0.2), (t0, 1.0)], cal[f'r{r}'])
        if SNAP:
            t = qadd([(t, 0.2), (t0, 1.0)], cal[f'r{r}'])
    body = layer('body', t, snap(cal['body.pre'], 1.0, feat.s[0]) if SNAP else None)
    body = qadd([(body, 1.0), (feat, 1.0)], cal['body+'])
    u = layer('up1', QT(esrgan.up2(body.q), body.s, body.z))
    u = layer('up2', QT(esrgan.up2(u.q), u.s, u.z))
    u = layer('hr', u)
    w, b, _ = next(it)						# the image: scale 1/255, zp 0
    y = qconv(u, w, b, None, (0.0, 1.0), None, 'last')
    return np.clip(rnd(y.real() * 255), 0, 255).astype(np.uint8)	# = y.q at 8 bit


def bias_correct(net, crops8, cal):
    """per-layer mean output shift caused by the weight rounding, measured on
    the quantised network's own activations (uncorrected pass) -> BC"""
    global COLLECT
    BC.clear()
    COLLECT = {}
    for x8 in crops8:
        qforward(net, x8, cal)
    BC.update({n: np.mean(v, axis=0) for n, v in COLLECT.items()})
    COLLECT = None
    return dict(BC)
