"""Check esrgan.forward against realesrgan-ncnn-vulkan: crop a photo, run
both, compare. realesrgan-ncnn-vulkan pads the input by 10 px (edge
replicate) and crops the 4x output.
  check_float.py photo.jpeg x y size out_dir  -> out_dir/crop.png, float.png
  check_float.py --cmp out_dir                -> PSNR of ncnn.png vs float.png"""
import os, sys, time
import numpy as np
from PIL import Image
import esrgan

MODEL = os.path.join(os.path.dirname(__file__), '../../../build/upscaler/models/upscayl-standard-4x')
PAD = 10


def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    return 10 * np.log10(255 ** 2 / mse) if mse else float('inf')


if sys.argv[1] == '--cmp':
    d = sys.argv[2]
    a = np.asarray(Image.open(os.path.join(d, 'ncnn.png')).convert('RGB'))
    b = np.asarray(Image.open(os.path.join(d, 'float.png')).convert('RGB'))
    print(f'ncnn vs numpy float: PSNR {psnr(a, b):.2f} dB, max |diff| {np.abs(a.astype(int) - b).max()}')
    sys.exit()

src, x0, y0, n, d = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
os.makedirs(d, exist_ok=True)
img = Image.open(src).convert('RGB').crop((x0, y0, x0 + n, y0 + n))
img.save(os.path.join(d, 'crop.png'))
x = np.asarray(img, np.float32) / 255
xp = np.pad(x, ((PAD, PAD), (PAD, PAD), (0, 0)), mode='edge')
net = esrgan.load(MODEL)
t = time.time()
y = esrgan.forward(net, xp)[4 * PAD:-4 * PAD, 4 * PAD:-4 * PAD]
print(f'float forward {xp.shape[1]}x{xp.shape[0]} in {time.time() - t:.1f} s')
Image.fromarray(np.clip(np.round(y * 255), 0, 255).astype(np.uint8)).save(os.path.join(d, 'float.png'))
