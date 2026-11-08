"""Per-tensor activation ranges of x4plus (float forward, min / max) over
crops of some photos, as the JSON export16.py takes (cal-x4plus.json is
checked in; the photos are not).
  calibrate.py <model base> out.json photo.jpeg..."""
import json, sys, time
import numpy as np
from PIL import Image
import esrgan, quant

PAD, N = 10, 64
SPOTS = ((0.25, 0.3), (0.6, 0.5), (0.8, 0.8))

net = esrgan.load(sys.argv[1])
crops = []
for p in sys.argv[3:]:
    im = Image.open(p).convert('RGB')
    for fx, fy in SPOTS:
        x0, y0 = int(fx * (im.width - N)), int(fy * (im.height - N))
        c = np.asarray(im.crop((x0, y0, x0 + N, y0 + N)))
        crops.append(np.pad(c, ((PAD, PAD), (PAD, PAD), (0, 0)), mode='edge').astype(np.float32) / 255)
t = time.time()
cal = quant.calibrate(net, crops)
json.dump({k: [float(a), float(b)] for k, (a, b) in cal.items()}, open(sys.argv[2], 'w'), indent=0)
print(f'{len(cal)} tensors from {len(crops)} crops in {time.time() - t:.0f} s')
