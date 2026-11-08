"""Calibrate on crops of some photos, run the uint8 simulation on a crop of
another, compare with the float output (PSNR, and PNGs to look at).
  eval_quant.py out_dir [pct] [--bc]"""
import glob, os, sys, time, pickle
import numpy as np
from PIL import Image
import esrgan, quant

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../build/upscaler')
PAD, N = 10, 64
out = sys.argv[1]
pct = float(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2][0] != '-' else None
os.makedirs(out, exist_ok=True)
photos = sorted(glob.glob(ROOT + '/photos/*.jpeg'))
net = esrgan.load(ROOT + '/models/upscayl-standard-4x')


def crop(path, fx, fy, n=N):
    im = Image.open(path).convert('RGB')
    x0, y0 = int(fx * (im.width - n)), int(fy * (im.height - n))
    return np.asarray(im.crop((x0, y0, x0 + n, y0 + n)))


def padded(a8):
    return np.pad(a8, ((PAD, PAD), (PAD, PAD), (0, 0)), mode='edge')


crops8 = [padded(crop(p, fx, fy)) for p in photos[1:] for fx, fy in ((0.3, 0.4), (0.7, 0.6))]
cal_file = os.path.join(ROOT, f'cal-{pct}.pkl')
if os.path.exists(cal_file):
    cal = pickle.load(open(cal_file, 'rb'))
else:
    t = time.time()
    cal = quant.calibrate(net, [c.astype(np.float32) / 255 for c in crops8], pct)
    print(f'calibrated on {len(crops8)} crops in {time.time() - t:.0f} s')
    pickle.dump(cal, open(cal_file, 'wb'))
if '--bc' in sys.argv:
    t = time.time()
    quant.bias_correct(net, crops8, cal)
    print(f'bias correction from {len(crops8)} crops in {time.time() - t:.0f} s')

for i, (fx, fy) in enumerate(((0.45, 0.45), (0.2, 0.7))):
    x8 = padded(crop(photos[0], fx, fy))
    t = time.time()
    yf = esrgan.forward(net, x8.astype(np.float32) / 255)
    y8f = np.clip(np.floor(yf * 255 + 0.5), 0, 255).astype(np.uint8)
    yq = quant.qforward(net, x8, cal)
    c = slice(4 * PAD, -4 * PAD)
    a, b = y8f[c, c].astype(np.float64), yq[c, c].astype(np.float64)
    psnr = 10 * np.log10(255 ** 2 / np.mean((a - b) ** 2))
    print(f'crop {i}: uint8 vs float PSNR {psnr:.2f} dB, mean |diff| {np.abs(a - b).mean():.2f}, '
          f'max {np.abs(a - b).max():.0f} ({time.time() - t:.0f} s)')
    Image.fromarray(y8f[c, c]).save(f'{out}/{i}-float.png')
    Image.fromarray(yq[c, c]).save(f'{out}/{i}-uint8.png')
    Image.fromarray(x8[PAD:-PAD, PAD:-PAD]).resize((4 * N, 4 * N), Image.BICUBIC).save(f'{out}/{i}-bicubic.png')
