# Real-ESRGAN x4plus on the MDLA (int16)

Upscayl's "upscayl-standard-4x" is byte-identical to Real-ESRGAN's
realesrgan-x4plus (RRDBNet, 351 convs, ~16.7 M parameters, ~19 M MACs per
input pixel; cezanne-photo uses the compact upscayl-lite-4x on the GPU because
x4plus takes ~2 hours there). Here it runs on MDLA0 in int16 with float
quality (2026-11-04).

## Quantisation results (vs the float model, 64 px photo crops)

| scheme | PSNR | look |
|--------|------|------|
| uint8 everywhere, min / max ranges | 38.4-38.8 dB | colour cast, grain in dark flat areas |
| uint8, 99.999 % percentile ranges | 40.3-41.3 dB | still grainy in the dark |
| uint8 + 16-bit tail only | 43.0 dB | less grain |
| int16 as the MDLA runs it (quant16) | 60.7 dB | identical (dark-area error 0.04) |
| **MDLA run (run16)** | **60.7 dB** (65.1 vs the simulation) | identical |

8-bit loses Real-ESRGAN's denoising (the residual trunk's activations), so
the MDLA's int16 mode (decoded for this: mainline/apu/README.md) is used:
int16 activations and weights, the weight scale bounded so the largest
calibration accumulator * 4 stays below 2^31 (the accumulator saturates at
32 bits), 11-15 bit weights in practice.

## In the camera app (2026-11-04)

cezanne-photo --upscale (Snapshot's "64MP" toggle) hands the picture to
**cezanne-upscaled**, falling back to realesrgan-ncnn-vulkan on the GPU when
the service is missing or fails:

- **/dev/cezanne-apu** (mainline/apu/cezanne-apu.c, in linux-cezanne,
  loaded at boot by modules-load.d): root only. The APU is powered while the
  device is open (one opener): both MDLA cores at 800 MHz (VPROC2 at 0.8 V
  while open), bus muxes on univpll_d5. CZ_APU_IOC_ALLOC: one DMA
  buffer (mmap), CZ_APU_IOC_RUN: command batches on one or both cores,
  waited for on the cores' done interrupts (since linux-cezanne 7.3.1-13:
  GIC SPI 413 / 414 in the DT's /cezanne-apu node; it looks for the end
  for spin_us = 1 ms first, as short runs end before a sleeping CPU is back;
  polled without the DT's interrupts or with use_irq=0). The MDLA reads and
  writes physical addresses, so the device is never given to users.
- **cezanne-upscaled** (root, socket-activated: /run/cezanne-upscale.sock,
  SOCK_SEQPACKET, group video, exits after 30 s idle): runs only the fixed
  network on pixels it is handed. Request {"width", "height", "order"} with
  two memfds (picture H x W x 3, result 4H x 4W x 3); replies {"tiles": n},
  {"done": k} per tile, {"ok": true, "seconds": s} or {"error": ...}. It
  holds a sleep inhibitor (systemd-inhibit child) while a job runs (wrapping
  the daemon in systemd-inhibit does not work: it closes the activated
  socket). DMA buffer from the APU's CMA pool above 4 GiB (1 GiB,
  mainline/apu/README.md "DMA pool"): ~750 MB for 579 px tiles, smaller
  tiles (386 px fits the default 512 MiB area) if that fails.
- **cezanne-photo**: memfds + SCM_RIGHTS, progress to stderr every 10 %;
  the 4x result is used in its mapping (no copy).

A 16 MP camera picture (3472 x 4620) at 2x: 17.7 min end to end with the
first runner (1036 tiles of 128 px), 13.0 min with a tile per core (432
tiles of 193 px; 40.9 dB from the first run's JPEG: tile seams + JPEG),
11.6 min with the patches shaped for the MAC array (1.0-7, same JPEG), 10.5
min at 800 MHz, 8.4 min (502 s) with the MDLA's own tiling (1.0-8, same
JPEG; VPU zones peak at 61 C), 7.3 min (440 s) with 386 px tiles (1.0-9 +
linux-cezanne 7.3.1-8; the pool had no 715 MiB for 579 px, which would be
~3 % faster; 193 px tiles: 500 s). Bigger tiles also mean fewer seams:
386 vs 193 px tiles 44.8 dB, the largest difference a visible seam of the
193 px tiling. GPU with the lite model: ~10 min, x4plus on the GPU: ~2 h.

## Files

- esrgan.py: ncnn model loader + float reference (matches realesrgan-ncnn-vulkan
  on the phone at 44.7 dB, the rest is its fp16 / border handling).
- quant.py: uint8 simulation (experiments, the table above).
- quant16.py: int16 quantisation (quantise() -> per-layer integer parameters,
  activation ranges x 2 headroom, accumulator x 4) and the integer-exact
  simulation of the MDLA run.
- calibrate.py -> cal-x4plus.json (checked in: per-tensor min / max over 24
  crops of 8 camera pictures; the pictures are not).
- export16.py: quantise + pack -> x4plus-int16.npz (33 MiB weights; the
  package builds it from realesrgan-ncnn-vulkan's model).
- mdlacmd.py: the command builder for the product (no MIUI data: the
  template words are constants), x4plus_mdla.py: the runner (layout,
  commands built once per tile size, tiling of whole pictures).
- cezanne-upscaled + units + PKGBUILD: the package cezanne-upscale.
- run16.py (dev, phone, root): one tile on every core, or a whole .npy
  picture, through x4plus_mdla (deps all / layers / none). eval_quant.py,
  check_float.py, check16.py: comparisons.

Dev workflow (laptop numpy has reference BLAS: use a venv,
`python -m venv build/qenv && build/qenv/bin/pip install numpy pillow`):

    calibrate.py <model base> cal-x4plus.json photo.jpeg...
    export16.py <model base> cal-x4plus.json build/upscaler/x4plus-int16.npz
    check16.py make build/upscaler/t16        # test tile + sim / float references
    (phone) run16.py x4plus-int16.npz in.npy out.npy
    check16.py cmp build/upscaler/t16 out.npy

## Speed

The runner (2026-11-04, "faster"): each core runs a whole tile of its own
(own buffers and command stream, shared weights) as **one submission**,
consecutive commands ordered by the 0x15c bits (mainline/apu/README.md); the
tail only computes the tile's own output region, its 4N stages in stripes
of 96 rows, so the buffers stay near the trunk's; the tile
size is picked per picture (pick_tile: least (T + 20)^2 work, at most 600
px: ~800 MB of DMA buffer for both cores; 400 / 200 / 136 / 72 px if
that fails).
Since 1.0-8 every layer region is **one command** the MDLA tiles itself
(tiling(): tiles of up to half the CBUF input, shaped for the array's
padding; all output channels at once where the weights fit;
mainline/apu/README.md "Spatial tiling"). Since 1.0-9 the whole trunk
lives in one buffer (x0 | c5 | x | o1..o4 | feat): the 1x1 adds write x
in place (a spatial tile's input is in the CBUF before its output is
written, and all output channels go at once), the tail from the body add
on runs in stripes, and the 4x output shares the trunk buffer (the
stripes' output stays below the trunk rows still to be read): ~0.58x the
memory at 193 px, 0.43x at 579 (the patch mode is gone).

| setup (84 px = 64 px tile + 2 x 10 context) | time per tile |
|-------|------|
| 208 MHz, one core, APU bus 208 MHz, per-layer submissions | 1.83 s |
| 700 MHz, both cores split per layer, bus 499 MHz | 0.32-0.35 s |
| 700 MHz, a tile per core, one submission per tile | 0.30 s |

| tile (one submission per core) | commands per tile | DMA buffer | per output pixel |
|------|------|------|------|
| 128 px, before (split per layer) | 23 k | 140 MB | 61 us |
| 128 px | 20 k | 160 MB | 52 us |
| 192 px | 42 k | 296 MB | 48 us |
| 193 px, CBUF-planned patches (46 x 62 .. 30 x 30) | 19 k | 279 MB | 47 us |
| 193 px, patches shaped for the MAC array (40 x 55 .. 24 x 24) | 24 k | 283 MB | 43 us |
| 193 px, the same at 800 MHz | 24 k | 283 MB | 39 us |
| 193 px, 800 MHz, the MDLA's own tiling (1.0-8) | 509 | 263 MB | 30 us |
| 193 px, one trunk buffer, in-place adds (1.0-9) | 548 | 152 MB | 30 us |
| 386 px (fits the default CMA area) | 644 | 378 MB | 27 us |
| 579 px (the APU pool above 4 GiB) | 740 | 715 MB | 26 us |

All bit-identical to per-layer submissions (tiled: the 147, 148, 212, 213
and 220 px test tiles byte-identical to the patch runner; 1.0-9: 147, 213,
220, 260, 386 and 579 px byte-identical to 1.0-8, the last two with 1.0-8
on one core, its 840 MiB from the pool). What limits it
(mainline/apu/README.md "Rate"): the MAC array. With patches the tile
reached ~408 int16 MAC / cycle on average (c5 438, the CO-32 layers
383-412: the ~10 us per command, CBUF-limited patches; the 1x1 adds,
memory-bound, 277). Tiled: 490 over all layers (every conv layer 485-509,
the adds 469), within a few % of the padded peak, so at 800 MHz the array
is the limit. The 10 px context cost (213 / 193)^2 = 22 % at 193 px,
(599 / 579)^2 = 7 % at 579 (a 16 MP picture: 6 x 8 tiles). Left: int8 (2x,
but the grain), adds without the dense 1x1 convs.

## Segmentation (portrait mode, 2026-11-04)

cezanne-upscaled's `segment` job: the salient object mask of a picture
(320 x 320 float32), U^2-Net (u2net from rembg's release, Apache-2.0, 44 M
parameters, 58.6 GMAC; since 2026-11-05, 1.0-14) in int16 on one MDLA core;
U^2-Net small (u2netp, 1.1 M parameters, 19.8 GMAC) when the full one's
150 MB DMA buffer cannot be had. cezanne-photo's `--portrait` uses it first
(portraitApu(): the mask snapped to the picture's edges by guided filters,
one blur strength) and the stereo pipeline only if it finds no subject;
Snapshot offers portrait on every camera with it.

- onnx2ops.py: the ONNX graph -> a flat op list (conv with fused ReLU,
  pool, resize to the size of another tensor, concat, add, sigmoid; the
  Shape / Gather arithmetic of the dynamic resizes traced to the tensor it
  reads) and its numpy float reference (1e-7 from onnxruntime).
- seg16.py: int16 per tensor (calibrated ranges x 1.25, cal-u2netp.json
  from the 9 test photos + mirrored), x4plus's conventions (input slice
  scales folded into the weights, accumulator margin 4, Q31 requant, ReLU =
  PReLU with factor 0), exact integer simulation. int16 vs float: mask
  probability within 0.07, IoU at 0.5 0.95-0.9995 (full u2net, split:
  within 0.015, IoU 0.992-0.9999). The CPU steps (add, resize, combine)
  are float32: 3x faster than float64 on the phone, and the simulation
  uses the same code.
- u2net_mdla.py: one DMA buffer (60 MiB): weights, ReLU tables, commands,
  activations reused by liveness. A concat is one buffer whose inputs are
  written straight into channel slices (a conv reading one alone uses the
  pitch). Dilated convs (d 2 / 4 / 8) as d x d standard convs over the
  decimated phases (pixel and row pitch x d; zero padding at a phase's edge
  is the dilated conv's); phases under 2 px (d 8 on 10 x 10) on the CPU
  (until 1.0-14 under 3 px: the full u2net's d 8 convs on 20 x 20 cost
  0.5 s there; 2 px phases run fine). Side outputs (CO 1) padded to 16 channels in buffers of
  their own. Pool, resize, add, copy, sigmoid in numpy between submissions:
  216 commands in 77 submissions, 89 CPU steps.
- Speed (u2netp): MDLA 56 ms (0.35 TMAC / s, one core), CPU glue 0.75 s
  with the float64 steps (not the uncached DMA buffer: it reads at ~1.4
  GB/s, 9 ms for a 320 x 320 x 64 tensor), ~1 s per job with the power-up.
  Mask vs float
  u2netp IoU 0.992-0.9995; vs the simulation within 6e-4 (not bit-exact:
  1-LSB requant differences at ties, as x4plus).
- The SoC reset (mainline/apu/README.md): the first version wrote the
  16-channel padded side output into a 1-channel slice of a 6-channel
  concat (12-byte pitch); found with a static check of every command's
  write range and pitch (pitches now multiples of 16 bytes, asserted).
- Full U^2-Net (1.0-14): convs with 512 / 1024 input channels need 147 /
  295 KB of weights per 16 output channels, more than the CBUF leaves (CI <=
  ~200 at lo 0x60; no native input-channel tiling found in the vendor's
  compiled nets). seg16.split(): a conv with CI > 128 becomes chunk convs
  over 128-channel slices of its input (no bias, no ReLU, each partial its
  own calibrated scale; they read the slice in place through the pitch) and
  a CPU combine (partials at their scales + bias, rounded, ReLU). 40 convs
  split; cal-u2net.json has 382 ranges. 805 commands (the d 8 / d 4 phases
  on 20 x 20 / 10 x 10 are most of them), DMA buffer 148 MiB (activations
  63). On the phone 0.86 s per run (MDLA 0.18, CPU 0.67: resize 0.16, add
  0.15, the two d 8 convs on 10 x 10, combine, pool), mask within 1e-3 of
  the simulation (the same 1-LSB requant ties, growing to ~10 LSB in late
  tensors).
- Masks (cmp sheets in build/seg): the full model picks the subject far
  more cleanly (the gnome on the sofa without the sofa, no stray lines,
  little leakage into the background) but, like u2netp less so, it knows
  upright scenes: the portrait bottle shot stored sideways lost its neck and
  cap (float model too); turned upright, either way, it keeps them.
  cezanne-photo therefore turns the 320 x 320 input upright by the
  picture's Exif orientation (Snapshot stores the frame of a phone held
  upright and tags the device orientation) and the mask back.
- People (1.0-15, the user: one button for objects, one tuned for people):
  rembg's u2net_human_seg (U^2-Net trained on people: the same graph and
  weight shapes as u2net, so the same split and runner), the segment job's
  `"model": "people"` (default "objects"), cezanne-photo `--subject
  people`, Snapshot's second portrait button. cal-u2net-human.json from the
  9 test photos plus public test pictures with people (scikit-image's NASA
  astronaut, OpenCV's messi5, basketball1 / 2 and four vtest.avi frames),
  mirrored; int16 vs float IoU 0.989-0.9998, mean error <= 0.0013, worst
  pixel 0.107 (the grey basketball frames). Float masks: only the person
  (not the astronaut's helmet or the shuttle, not the ball), every
  pedestrian, nothing on the bottle (the stereo fallback then).
- Portrait test pair (mainline/camera/dumps/calibration/portrait): APU path
  16 s end to end (PNG in / out), the stereo path 291 s on the same phone
  state (its docs said ~25 s: not investigated).

## Depth (portrait blur, 2026-11-05)

cezanne-upscaled's `depth` job: the relative depth of a picture (256 x 256
float32, larger = nearer, arbitrary scale and offset), MiDaS v2.1 small
(model-small.onnx from its release, MIT; EfficientNet-Lite3 encoder with
24 depthwise convs, RefineNet decoder; 4.6 GMAC) in int16 on one MDLA
core. cezanne-photo's portraitApu() grades the blur with it as the stereo
path does with disparity: the depth snapped to the picture's edges (guided
filter at a quarter of the resolution), the focus the subject's median
depth (inside the U^2-Net mask), the step from the far background (10th
percentile) to it the scale, blur |d - focus| / (step / 2) clamped to
0.35 .. 1 outside the subject: things near the subject stay sharper, the
far background (and things in front) blur fully. Without the job: one
strength as before.

- midas.py: the ONNX graph -> a flat op list (batch norm folded; the
  explicit Pad ops before stride 2 folded into the convs; ReLU6; the
  RefineNet units' lone ReLU; add; opset 10 Resize = 2x bilinear,
  asymmetric), its float reference (1e-6 of the range from onnxruntime),
  int16 (seg16.py's conventions; ReLU6 outputs at a scale whose int16 top
  is at most 6, so saturation is the clamp; the decoder's 3 x 3 convs with
  512 / 256 / 136 inputs split into 128-channel chunks + a CPU combine),
  integer simulation, export. cal-midas.json from the 9 test photos and
  the 8 people pictures, mirrored. int16 vs float: correlation 0.99986+,
  mean error 0.02-0.26 % of the depth range.
- depth_mdla.py: u2net_mdla.py's layout (one DMA buffer, buffers reused by
  liveness: 41 MiB, activations 9), stride-aware tiling, depthwise
  commands (mainline/apu/README.md "Depthwise and strided convs"): they
  add no bias in int16, so each writes '<out>.nobias' and a CPU step
  (dwfix) adds the bias, applies ReLU6 and rounds to the output's scale.
  Two hardware limits found on the way and kept: a depthwise tile row of
  Tw x C <= 4096, all-output-channel mode only up to 512 channels. 110
  commands, 129 steps.
- On the phone: 0.125 s per run (MDLA 18 ms, CPU glue 104 ms), the job
  0.27 s; every layer within 0.34 % of its range of the simulation, the
  depth within 0.064 %; vs onnxruntime's float model correlation 0.999999.
  A portrait with depth: 5.5 s for the 16 MP bottle shot (JPEG in / out),
  under 3 s for small pictures.

## Video (2026-11-05)

`cezanne-video-upscale [--scale 2|4] in out.mp4` (the gallery's "Upscale 2x
(AI)" for videos up to 1080p): the compact Real-ESRGAN, Upscayl's
upscayl-lite-4x (= realesr-general-x4v3, SRVGGNetCompact: 34 3x3 convs, 64
channels, per-channel PReLU, PixelShuffle 4 + the input nearest 4x; 1.2 M
MAC per input pixel, ~1/60 of x4plus), int16 on both MDLA cores.

- compact.py: ncnn loader (PReLU slopes are separate float32 layers, -1.6 ..
  1.9), float reference, int16 (cal-compact.json from 27 crops of the test
  photos; x4plus's conventions, the PReLU's negative factors per channel in
  the 0x16c / 0x170 table), integer simulation. int16 vs float 77-81 dB, at
  most 0.8 of a level. 2x is built in: the 4x result averaged over 2 x 2
  blocks is linear, so the last conv's 48 outputs averaged in groups of 4
  give a 12-output conv + PixelShuffle 2 (exact to 5e-16 before clipping).
- compact_mdla.py: tiles of Th x Tw + 16 px context (rectangular, the frame
  split evenly: 1080p as 8 of 544 x 480, 1.14x the frame's work), one per
  core, 34 chained commands each; per core A / B shared and two sets of IN /
  OUT / commands, so the CPU finishes a group (PixelShuffle + nearest input,
  compact.finish) and stages the next while the MDLA runs (the ioctl
  releases the GIL): 1080p -> 2160p 5.6 -> 3.5 s per frame, the MDLA's limit
  (~0.8 TMAC / s on both cores). Outputs match the simulation within 1 level
  (0.03 % differ: requant ties).
- cezanne-upscaled `frames` session: the network built once per video, a
  frame per request through two memfds; clients that come in meanwhile
  (portrait, 64MP) are served between two frames (tested: a segment job
  3.3 s after it came in, the video went on).
- cezanne-video-upscale: ffmpeg decodes (rotation applied, constant rate at
  the stream's average, cropped at the centre to multiples of 8 px), the
  frames go to the daemon, ffmpeg encodes (audio copied); the previous frame
  is encoded while the APU works on the next. <out>.part renamed at the end.
  Encoder (1.0-13): the phone's H.264 encoder by default (cezanne-venc
  through h264_v4l2m2m, ~0.2 bit per pixel: 2864 x 2160 at 30 fps = 37
  Mbit/s), `--encoder x264` for libx264 CRF 18. The same 87-frame clip: 237
  s with the hardware encoder, 266 s with x264 (whose 4K encode competes
  with the APU's CPU work); hardware vs x264 output 47.4 dB. A 2876 px wide
  frame (not a multiple of 16) timed out in VENC and left it wedged until
  a reboot: hence the crop (2x of a multiple of 8 = a multiple of 16).
- A 3 s 1080 x 1438 recording (87 frames): 2876 x 2160 in 282 s (2.7 s per
  frame on the APU); the result scaled back down 30.7 dB from the original
  (noise gone, edges and text sharper). About 100 s per second of 1080p
  video at 30 fps. Not judged yet: flicker between frames (each frame alone).
