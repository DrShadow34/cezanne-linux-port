"""Test tile for run16.py and the comparison with the simulation / float.
  check16.py make dir                 -> dir/in.npy, sim.npy (+ sim tensors), float.npy
  check16.py cmp dir out.npy          -> PSNR of the MDLA output vs sim / float
  check16.py cmpt dir name dump.npy   -> one intermediate tensor vs the sim"""
import glob, json, os, sys
import numpy as np
from PIL import Image
import esrgan, quant16

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../build/upscaler')
PAD, N = 10, 64


def psnr(a, b, peak=255):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 10 * np.log10(peak ** 2 / mse) if mse else float('inf')


cmd, d = sys.argv[1], sys.argv[2]
if cmd == 'make':
    os.makedirs(d, exist_ok=True)
    photos = sorted(glob.glob(ROOT + '/photos/*.jpeg'))
    im = Image.open(photos[0]).convert('RGB')
    x0, y0 = int(0.45 * (im.width - N)), int(0.45 * (im.height - N))
    x8 = np.pad(np.asarray(im.crop((x0, y0, x0 + N, y0 + N))), ((PAD, PAD), (PAD, PAD), (0, 0)), mode='edge')
    np.save(f'{d}/in.npy', x8)
    net = esrgan.load(ROOT + '/models/upscayl-standard-4x')
    cal = {k: tuple(v) for k, v in json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cal-x4plus.json'))).items()}
    keep = {}
    y = quant16.qforward16(net, x8, cal, hook=lambda n, t: keep.__setitem__(n, t) if n == 'r0.0' or n.endswith('.2') else None)
    np.save(f'{d}/sim.npy', y)
    for n, t in keep.items():
        np.save(f'{d}/sim-{n}.npy', t.astype(np.int16))
    yf = esrgan.forward(net, x8.astype(np.float32) / 255)
    np.save(f'{d}/float.npy', np.clip(np.floor(yf * 255 + 0.5), 0, 255).astype(np.uint8))
elif cmd == 'cmp':
    y = np.load(sys.argv[3])
    c = slice(4 * PAD, -4 * PAD)
    if y.shape[0] != np.load(f'{d}/sim.npy').shape[0]:	# the runner's tile part only
        y = np.pad(y, ((4 * PAD, 4 * PAD), (4 * PAD, 4 * PAD), (0, 0)))
    for ref in ('sim', 'float'):
        r = np.load(f'{d}/{ref}.npy')
        print(f'MDLA vs {ref}: PSNR {psnr(y[c, c], r[c, c]):.2f} dB, mean |diff| {np.abs(y[c, c].astype(int) - r[c, c]).mean():.3f}, '
              f'max {np.abs(y[c, c].astype(int) - r[c, c]).max()}')
    Image.fromarray(y[c, c]).save(f'{d}/mdla.png')
    Image.fromarray(np.load(f'{d}/float.npy')[c, c]).save(f'{d}/float.png')
else:
    t = np.load(sys.argv[4]).astype(np.int64)
    r = np.load(f'{d}/sim-{sys.argv[3]}.npy')
    dd = t - r
    print(f'{sys.argv[3]}: mismatches {(dd != 0).sum()} of {dd.size}, |diff| max {np.abs(dd).max()}, '
          f'range {r.min()}..{r.max()}, PSNR {psnr(t, r, 65535):.1f} dB (int16 scale)')
