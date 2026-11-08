"""Dev check of the MDLA runner (phone, root, cezanne-apu.ko loaded): one
tile from a .npy (on every core at once), or a whole picture.
  run16.py model.npz in.npy out.npy [deps] [cores]
      in: uint8 N x N x 3, the tile with its 10 px context; out: 4(N-20)^2 x 3
      deps: all (default) | layers | none
  run16.py model.npz --pic in.npy out.npy [tile] [deps]
      a whole picture (uint8 H x W x 3)"""
import sys, time
import numpy as np
from x4plus_mdla import X4Plus, load_model, PAD

model = load_model(sys.argv[1])
if sys.argv[2] != '--pic':
    x8 = np.load(sys.argv[2])
    deps = sys.argv[4] if len(sys.argv) > 4 else 'all'
    cores = int(sys.argv[5]) if len(sys.argv) > 5 else 2
    t = time.time()
    net = X4Plus(model, tile=x8.shape[0] - 2 * PAD, cores=cores, deps=deps)
    print(f'{net.ncmd} commands per tile, {len(net.streams[0]["layers"])} submissions, '
          f'{net.size / 2 ** 20:.0f} MiB, set up in {time.time() - t:.1f} s')
    for _ in range(3):
        t = time.time()
        ys = net.run_tiles([x8] * cores)
        print(f'{cores} tiles in {time.time() - t:.3f} s, cores agree: {all(np.array_equal(ys[0], y) for y in ys)}')
    net.close()
    np.save(sys.argv[3], ys[0])
else:
    img = np.load(sys.argv[3])
    tile = int(sys.argv[5]) if len(sys.argv) > 5 else 192
    deps = sys.argv[6] if len(sys.argv) > 6 else 'all'
    t = time.time()
    net = X4Plus(model, tile=tile, deps=deps)
    print(f'{net.ncmd} commands per tile, {net.size / 2 ** 20:.0f} MiB, set up in {time.time() - t:.1f} s', flush=True)
    t = time.time()
    y = net.upscale(img, progress=lambda d, n: print(f'\r{d}/{n}', end='', flush=True))
    print(f'\n{img.shape[1]}x{img.shape[0]} -> {y.shape[1]}x{y.shape[0]} in {time.time() - t:.1f} s')
    net.close()
    np.save(sys.argv[4], y)
