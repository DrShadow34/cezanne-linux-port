# APU (AI accelerator) on mainline

MT6885 APU 3.0: two MDLA 1.5 cores (MediaTek Deep Learning Accelerator,
fixed-function, command driven), three VPU (Cadence Vision P6 DSPs, need
firmware), EDMA, a 1 MiB VLM / TCM SRAM at 0x1d000000 behind the
"reviser", its own IOMMUs (0x19010000, 0x19015000) and NoC. MIUI drove it
with the apusys BSP drivers (src/kernel-cezanne/drivers/misc/mediatek/
apusys, GPL) and NeuroPilot (proprietary: libneuron_*, the NNAPI HAL with
the Neuron compiler, `neuron::mdla::V1_X`).

Goal (user, 2026-11-04): run the camera app's AI upscaling (Real-ESRGAN
compact, ncnn-Vulkan on the GPU today: 9 s per 512x512 tile) on the MDLA.

## State (2026-11-04)

- **Power-up works** (apuprobe.ko, bring-up module): MDLA0 answers
  (G_REV 0x100, COREINFO 2 = two cores, CDMA0 0x1c0 = command size).
- **Commands run, bit-exact**: MediaTek's two MDLA test patterns from
  MIUI's libmdla_ut.so give exactly their golden output:
  conv2d_depthwise (31 KiB out) in 116 us, MaxPower_1 (3x3 conv + BN
  tables, 2 MiB out) in 8.8 ms, at 208 MHz (OPP table: 700 MHz at the
  0.75 V lk leaves on VPROC2, 800 MHz at 0.8 V).
- Standard 3x3 conv decoded and verified (below): any frame size as
  batched patch commands, any input channel count (RGB as 3 channels with
  pitch 4), with the PReLU stage in the same command (bit-exact). All
  layer kinds of the upscaler run natively and chain over a frame
  (re/e11_chain.py).
- int16 mode decoded (below); Real-ESRGAN x4plus (Upscayl's standard
  model) runs in int16 at float quality: upscaler/README.md.
- Both cores at 700 MHz (APUPLL), APU bus clocks at 499.2 MHz: x4plus
  84 px tile 1.83 s -> 0.32-0.35 s, bit-identical output. 800 MHz since
  the evening (VPROC2 raised to 0.8 V while the device is open).
- The MDLA's own spatial tiling works (below, "Spatial tiling"): a whole
  layer region is one command at ~490 int16 MAC / cycle, near the array's
  limit; x4plus 509 commands per tile instead of ~21000, the 193 px tile
  pair 2.88 -> 2.25 s at 800 MHz, byte-identical output.
- The DMA buffer comes from a 1 GiB CMA pool above 4 GiB (below, "DMA
  pool", linux-cezanne 7.3.1-8): x4plus tiles of 579 px when the pool
  has room for 715 MiB, else 386 px (378 MiB), instead of 193.
- Segmentation (2026-11-04, night): U^2-Net small (u2netp, rembg's salient
  object model) in int16 on one core, 56 ms of MDLA time (~1 s with the CPU
  glue and the power-up), for the camera's portrait mode
  (upscaler/README.md "Segmentation"). Dilated convs as decimated standard
  convs (pixel / row pitch x d).
- Video (2026-11-05): the compact Real-ESRGAN (upscayl-lite-4x) in int16 on
  both cores, 1080p -> 4K in 3.5 s per frame; cezanne-video-upscale and the
  gallery (upscaler/README.md "Video"). Per-channel PReLU with negative and
  > 1 factors works through the 0x16c table.
- **Trap: output pixel pitches must be multiples of 16 bytes.** A 16-channel
  write into a 12-byte pitch (6 int16 channels) reset the whole SoC at once
  (2026-11-04: no hang, no log, btrfs lost its last writes); a plain MDLA
  hang only wedges the APU.
- In the camera app (2026-11-04): driver cezanne-apu.c (/dev/cezanne-apu,
  linux-cezanne 7.3.1-5) + cezanne-upscaled (package cezanne-upscale):
  upscaler/README.md "In the camera app". apuprobe.c stays the RE tool
  (debugfs; never load both). Done interrupts instead of polling since
  linux-cezanne 7.3.1-13 (GIC SPI 413 / 414, 1 ms look first; NOTES "MDLA
  done interrupts"). Open: int8, the VPUs.

## Power-up sequence (apuprobe.c)

From hal_config_power.c / apu_power_clock.c / clk-mt6885-pg.c / mnoc /
reviser of the BSP:

1. Rails: MT6359 VPROC1 = vvpu, VPROC2 = vmdla, VSRAM_MD = vsram_apu (MIUI
   DT); lk leaves them at 0.75 / 0.75 / 0.85 V, enabled. Vcore = VGPU11.
2. topckgen ipu_if_sel, dsp_sel, dsp7_sel on (vcore, conn, IOMMU clocks).
3. RPC (0x190f0000) init, once: SW_TYPE per memory group, PWR_CON bit 1
   for VPU0-2 / MDLA0-1, TOP_SEL |= 0x9e | BIT(10); sleep request only if
   the conn is up (never request sleep twice).
4. Conn on through the SPM: POWERON_CONFIG_EN unlock, EXT_BUCK_ISO (0x39c)
   &= ~0x21, SPM_CROSS_WAKE_M01_REQ (0x670) |= 1, wait OTHER_PWR_STATUS
   (0x178) bit 5, then RPC INTF_PWR_RDY (0x44) bit 0 (reads 0x3).
5. Clock gates (CLR at +8): apu_vcore 0xf, apu_conn 0x3fffe.
6. ATF: SMC 0xc2000519 (MNOC) op 0 = infra2apu SRAM enable; SMC
   0xc200051e (APUSYS) op 0 = reviser boundary (bits 33:32 added to
   MDLA / VPU / EDMA addresses, command fetches and data; cezanne-apu: 1,
   its DMA pool at 0x1_9000_0000, below "DMA pool"; MIUI 3 for its 34-bit
   IOVAs). The reviser's registers are secure (read as 0).
7. MDLA clock before dsp6_sel is enabled (apuprobe mdla_mhz, default
   700): APUPLL (apmixed id 16, used only by the APU muxes; lk leaves 1.7
   GHz) set to 2 x the rate, dsp6_sel (both MDLA cores) reparented to
   apupll_ck = APUPLL / 2. BSP OPP: MDLA 700 MHz at 0.75 V (VPROC2 as lk
   leaves it), 800 at 0.8 V: cezanne-apu (mdla_mhz=800) raises VPROC2 to
   0.8 V before the clock while the device is open and back to 0.75 V on
   close (DT: buck_vproc2 750-800 mV, always-on; without constraints the
   regulator core refused voltage changes); VSRAM_MD stays at lk's 0.85 V
   (BSP: >= 0.825 V when vmdla > 0.75). A 16 MP picture at 800 MHz: VPU
   thermal zones peak at 57 C. fast_bus (default on): dsp_sel (conn),
   dsp7_sel (IOMMU), ipu_if_sel reparented from univpll_d6_d2 (208) to
   univpll_d5 (499.2; OPP at VPROC1 0.75 V: conn 550, IOMMU 499; ipu_if 550
   at Vcore 0.725 V). **Never clk_set_rate() these muxes**: MediaTek muxes
   have CLK_SET_RATE_PARENT, and asking dsp_sel / ipu_if_sel for 550 and
   dsp7_sel for 499 MHz re-tuned UNIVPLL to 2750 MHz and TVDPLL to 499:
   the USB network link dropped and the APU bus died a load later
   (reboot). Reparent onto fixed dividers only.
8. MDLA0 / MDLA1: dsp6_sel on, RPC SW_FIFO_WE = (6 | 7) | BIT(4) (after
   TOP_CON bit 31 clears), wait INTF_PWR_RDY bit 6 / 7; cfg (0x19034000 /
   0x19038000) CG_CLR all. MDLA1 = MDLA0 + 0x4000 (cfg, biu 0x19039000, cmd
   0x1903a000, IRQ 414); identical results on both.
9. mdla_reset() per core: INTP2 = 0x1fffff & ~SWCMD_DONE, ENG0-2 = 0, ENG11 = 0x600.
   **Not** the BSP's MDLA_AXI_CTRL / AXI1_CTRL bits 1 + 10 ("ION"): with
   them the MDLA's accesses go through the APU IOMMU (no page table here)
   and the first command never finishes (IDLE bit 23 busy, no IOMMU
   fault); without them it reads / writes physical addresses.

Off: MDLA RPC FIFO_WE = 6, wait bit 6 clear; SMC MNOC op 1; CROSS_WAKE
&= ~1; RPC TOP_SEL |= 0x9e, TOP_CON |= 1, wait bit 0 clear; clocks off.

## Running commands

Commands are 0x1c0-byte register images, one per layer / tile, in memory;
the CDMA fetches them. apuprobe's debugfs run takes "<offset hex> <count>
[core]" or "<off0> <n0> <off1> <n1>" (both cores at once, e.g. a layer's
patches split in two; each part its own ids 1..n and finish bit). Issue (mdla_issue_ce): INTP0 bit 5 (CDMA FIFO
empty) set, write CDMA1 (0x514) = address, CDMA2 (0x518) = count, CDMA3
(0x51c) = 1. The last command needs SWCMD_FINISH_INT_EN (bit 24 of word
0x154, MREG_CMD_TILE_CNT_INT); completion = INTP0 bit 2 (SWCMD_DONE),
write-1-to-clear. FIN3 (0x584) is the finished command's id (word 0x150),
not a count.

Buffer addresses in a command (libmdla_ut.so GetOffsetFromType_v1_5):

| word | buffer |
|------|--------|
| 0x000 | input activation |
| 0x004 | weights |
| 0x008 | output activation |
| 0x050 / 0x054 | quant table mul / add |
| 0x0b4 | (type 12) |
| 0x0d4 / 0x0d8 | per-channel coefficient table mul / add (BN) |
| 0x0e4, 0x108, 0x10c | (types 7-9) |
| 0x160-0x170 | (types 13-17: requant mul, PReLU table / shift, ...) |

### Layer fields decoded so far (the two test commands)

| word | depthwise test | MaxPower | meaning |
|------|----------------|----------|---------|
| 0x00c | 0x007e0001 | 0x00430001 | input H << 16 \| batch |
| 0x010 | 131 | 67 | input W |
| 0x018 | 0x00060020 | 0x00030100 | KH << 16 \| Cout |
| 0x01c | 0x00010007 | 0x00400003 | Cin per group (1 = depthwise) << 16 \| KW |
| 0x020 | 0x001f0001 | 0x00400001 | output H << 16 \| batch |
| 0x024 | 0x00200020 | 0x01000040 | Cout << 16 \| output W |
| 0x028 / 0x02c / 0x030 | 0x7c00 / 0x400 / 0x20 | | output size / row pitch / channel pitch |
| 0x034 / 0x038 / 0x03c | 0x80f40 / 0x1060 / 0x20 | | input size / row pitch / channel pitch |
| 0x04c | 0x600 | 0x2400 | weight block size |
| 0x058 | 0x46a9abe4 | 0x40000000 | requant multiplier (Q31 fraction) |
| 0x05c | 0x7f6 | 0x6d00 | [7:0] output zero point, [11:8] extra right shift |

Depthwise test, checked in numpy: input uint8 HWC (126x131x32), weights
uint8 [ky][kx][c] (6x7x32, no zero point, block padded to 0x600), stride
4, no padding, bias = QuantTableAdd int32 per channel, output uint8 =
clamp(round((acc + bias) * mult / 2^(31 + shift)) + zp): with the
multiplier rounded to 16 bits (0x46aa >> 22) 19 of 31744 bytes are one
lower in hardware, all at rounding ties (.5000x); the exact rounding is
open (irrelevant for images). MaxPower is random int16 stress data
(saturated output), no use for semantics.

### Standard convolution: decoded and verified (2026-11-04, re/mdla.py)

Built from reid layer 72 (3x3 256 -> 256), checked bit-exact (or +-1 at
rounding ties, ~0.04 %) against numpy on random, well-scaled data up to
60x60x64 -> 64 per command, and on whole frames (96x96, 100x90) as one
command per patch:

- Weights uint8 [co][ky][kx][ci] packed per 16-output-channel tile into two
  blocks (0x04c = block size = K*K*CI*8) interleaving input-channel pairs:
  offset = cot*2*blk + part*blk + a*K*K*32 + tap*32 + co*2 + lo with
  ci = 4a + 2 part + lo (re/mdla.py pack_conv_weights). 0x04c is rounded up
  to 256 bytes (re/mdla.py wblock; every compiled layer: CI 16 K 3 0x500,
  CI 24 K 1 0x100, CI 3 pitch 4 0x200): an unrounded 0x480 for CI = 16
  hung the APU, the rounded value works.
- Few input channels (the RGB layer): channel pitch 0x03c = 4 (also the
  row / frame pitches 0x038 / 0x034 / 0x19c), 0x014 = CI*16 = 0x30, CI 3 in
  0x01c; the MACs still read channel 3, so its weights must equal zw (then
  w - zw = 0 whatever the pad byte holds). Input row pitches must be a
  multiple of 16 bytes (RGBX 90 px = 360 B gave garbage, 92 px works; int16
  RGBX 213 px = 1704 B the same: streaks all over the x4plus output).
- acc = sum(xpad * (w - zw)) + bias[co] (QuantTableAdd int32 at 0x054);
  out = clamp(round(acc * m16 / 2^(15 + sh)) + zp_out), m16 = word 0x058
  rounded to 16 bits. Word 0x05c: [22:15] weight zero point zw, [14:12]
  output type (0 uint8, 6 int16), [11:8] shift sh, [7:0] output zero point. No input zero point: padding uses the
  pad value 0x0c4 [23:16], so zx folds into the bias exactly
  (bias -= zx * sum(w - zw)).
- 0x0c4 [15:0]: pad nibbles bottom / top / right / left ([15:12] [11:8]
  [7:4] [3:0]); 0x0c0 stride (h << 4 | w).
- Spatial tiling inside a command: 0x074 = 1 << 24 | Cout tiles << 16 |
  H tiles << 8 | W tiles, 0x078 = their product. Per dimension, To output
  rows a tile: 0x08c / 0x09c To << 16 | Tw, 0x080 input To*s, 0x07c
  (To - 1)*s + K, 0x094 / 0x0a0 (and 0x098 / 0x0a4 for W) E = tiles*To -
  out, 0x084 (E*s + pad_bottom) << 16 | pad_top, 0x088 the same for W.
- Internal buffer (CBUF, 128 x 4 KiB banks), 0x124 = hi << 16 | lo; the
  full map is below ("CBUF", 2026-11-04). r72's lo = 16 broke inputs above
  ~64 KiB; lo = 0x40 works up to 60x60x64 uint8. 0x0c8 = 0x40c (Cout-tiled
  mode; 0xc gave other wrong results). 0x120 = VLM / TCM placement between
  fused layers, 0 for DRAM.
- 0x15c bits 15 / 16 (SWCMD int / wait for the previous command) must be
  clear on a lone command, or it waits forever (the BSP clears the wait
  bit on a batch's first command). As the compiled nets chain them (first
  command 15, middle 15 + 16, last 16), a whole network runs as one batch:
  x4plus tiles of 6400 to 42000 patch commands per core in one submission,
  identical to one submission per layer (2026-11-04). Bits only at layer
  boundaries work as well and are no faster: the MDLA does not overlap
  commands anyway.
- Frames: one command per patch (<= 60x60), input / output addresses at the
  patch origin with the frame's row pitches (0x038 / 0x02c); halos come
  from the neighbours, padding only at frame edges. 60x60x64 -> 64 in
  734 us at 208 MHz (~870 MAC / cycle).
- A hang (bad geometry) wedges the APU: the MDLA soft reset and an MDLA
  domain power cycle do not recover it (the next good command hangs too),
  the conn will not sleep: reboot.

- Batches: consecutive commands, ids 1..n in word 0x150, SWCMD_FINISH_INT_EN
  only on the last, CDMA2 = n: 4 and 9 patch commands per submission
  correct (re/mdla.py run_batch); 120x120x64 -> 64 in 2.9 ms at 208 MHz.
  CO = 48 (three channel tiles, the upscaler's last layer) correct.
- Fusion (compiled networks): a consumer command has input address 0,
  0x118 low nibble 1 and 0x0ac = 2, and reads the previous command's
  output from the CBUF (0x120 / 0x0c8 / 0x11c describe the placement);
  producers have 0x118 low nibble 0. Copied as is, such commands hang
  standalone (the add below did, twice).

Element-wise add (preview layer 9, f4000020): second input at 0x004 with
its geometry in 0x040 / 0x044 / 0x048 (+ size 0x1a0), per-input
multipliers 0x060 / 0x064, zero points 0x068 ([23:16] [15:8] [7:0]),
shift 0x06c, op 0x0cc (5 = add), operands 0x0d0. Standalone runs hung
(fused producer: 0x118 f040, 0x11c c042, CBUF fields), also with 0x0c8 /
0x120 / 0x124 cleared.

### PReLU: a stage of the conv command (2026-11-04, re/e8, e9)

Found in libaibc_tuning.so (8 compiled int16 nets of 38 commands, 25 conv
+ PReLU each, plain DRAM commands); decoded for uint8 with re/e8_prelu.py
and checked bit-exact with re/e9_prelu_rand.py (0 mismatches in 4 x 102400
outputs, modelled on the hardware's own conv output: re/mdla.py ref_prelu).
No fusion and no extra command: the conv command runs it after the
requantisation, at no MAC cost.

- Enable: 0x070 |= 0x300000 (bits 20 + 21), 0x0cc = 0x30 (int16 nets 0xf0:
  bits 7:6 are the data type), 0x0d0 = 0x0f000000, 0x14c = 0x18100000 (as
  compiled, meaning open), 0x0e0 [3:0] = 7.
- Input: the conv's clamped uint8 output y (0x05c zero point applied);
  d = y - zp_in with zp_in = 0x068 [7:0].
- d >= 0: out = round(d * M / 2^(11 + S)) with M = 0x0f0 (Q31, signed), S =
  0x0e8 [21:17] (bit 16 is not part of it: 38 and 39 << 16 give the same).
- d < 0: the same with per-channel M = int32 table at 0x16c, S = uint8 table
  at 0x170 (alpha * the positive scale folded in; M may be negative).
- out += zp_out (0x0e0 [11:4]), clamp to 0..255; rounding half up.
- Compiled nets keep 25 bits of M (low byte 0 / 0x80); re/mdla.py q31,
  prelu_fields, prelu_tables build the fields. In the int16 nets alpha =
  (M_c / 2^S_c) / (M_pos / 2^S_pos) gives the trained slopes (-0.5..1.0).

Word 0x070 by the compiled nets: bit 16 conv, 20 + 21 PReLU, 23 fused
element-wise op after the conv (0x0cc low nibble = op, multiplier 0x060,
shift 0x06c, per-channel add table 0x0d8, e.g. preview 79), 24 LUT
activation (table 0x0e4, 0x0e0 = 1, 0x0e8 = 0x40), 26 element-wise main op,
27 pooling; bit 1 is set on many convs (probably ReLU, untested). The
NeuroPilot "PReluEngine" (prelu_params->...) is the VPU backend's, not the
MDLA's.

### int16 mode (2026-11-04, re/e12-e16)

From libaibc_tuning.so's int16 nets, verified against numpy (+-1 at ties):
- Our uint8 conv command plus: 0x05c [14:12] = 6 (zero points 0: clear the
  template's zw), 0x0c8 |= 3 (low nibble 0xc -> 0xf; element-wise 0 -> 3),
  0x0cc [7:6] = 3 (0xc0, PReLU 0xf0), 0x0fc = 1, 0x15c [11:0] = 0x555 (all
  uint8 commands: 0x6aa). Pitches 0x02c-0x03c stay in elements; sizes
  0x19c / 0x1a4 and the weight block 0x04c are bytes (re/mdla.py
  int16_mode, conv_geometry esz=2).
- Weights int16, the uint8 order with single channels in place of pairs:
  slot = cot * 2 blk + (ci & 1) * blk + (ci >> 1) * K*K*16 + tap * 16 + co
  (re/e13_w16probe.py: every slot holding its index, one-hot input).
  0x04c = K*K*CI*16 rounded up to 256 with CI rounded to the MAC group (2;
  uint8: 4), not the input pitch.
- Requant: out = clamp(round(acc * M / 2^(31 + sh))) with the full Q31
  multiplier (uint8 uses 16 bits of it). The accumulator saturates at 32
  bits (re/e16: sums past 2^31 come out clamped): keep weight scales so
  |acc| stays well below 2^31.
- PReLU stage the same fields, factor M / 2^(19 + S) for both branches
  (uint8: 11 + S), zero points 0 (re/e14_prelu16.py).
- ~445 MAC / cycle (uint8 ~870).
- Channel slices (re/e15_rdb16.py, a whole RDB): input pitch 0x03c > CI and
  output pitch 0x030 > CO with channel offsets in the addresses read and
  write only their channels; K = 1 convs work (residual adds).
- The MDLA-internal spatial tiling (0x074 th / tw > 1) needs a second
  input buffer: "Spatial tiling" below.

### CBUF map (2026-11-04, re/e17_cbuf.py, re/e18_plan.py; int16)

Found with sweeps of single convs (sizes, channels, lo, hi, padding)
against numpy; every pass / fail of the sweeps fits:

| banks (4 KiB) | holds |
|---|---|
| [0, lo) | the input window, each side + the conv's padding rounded up to 8 px: ceil8(H + pad_t + pad_b) x ceil8(W + pad_l + pad_r) x CI x 2 B. At most 96 banks: lo 0x40 holds 64, lo 0x60 and anything above holds 96 (0x58 still 64). |
| [lo, hi) | the weights of one 16-output-channel tile (2 x 0x04c B) |
| [hi, hi + tile) | the next tile's weights: the MDLA double-buffers them (only used with more than 16 output channels) |
| 0x7e, 0x7f | not usable: anything there fails |

### Spatial tiling (2026-11-04, evening, re/e21_tiling.py; int16)

One command over a whole frame (or layer region), the MDLA walking th x tw
output tiles of To x Tw px itself (fields in "Standard convolution":
0x074 / 0x078 and the per-dimension words 0x07c .. 0x0a4, the same
formulas as MediaTek's compiled nets, which tile 354 of their 586
commands). Exact against numpy (int16 and uint8 paths of e21) for even and
uneven tiles, pad 1 and 0, 1x1 convs, CI 4 .. 192, CO 32 .. 64, frames up
to 213 x 213 x 192.

- **Second input buffer**: the MDLA loads tile k + 1's input while it
  computes tile k. Without 0x120 [23:16] the load lands on the tile in use:
  output tile k came out as the conv of input tile k + 1, only the last
  tile right. 0x120 = bank << 16, the compiled nets' 0x300000 with lo =
  0x60: buffers [0, 0x30) and [0x30, 0x60). Input addresses wrap at bank
  0x60 (a buffer reaching past it corrupts the other; more lo does not
  help), so a tile's input window, ceil8(To + K - 1) x ceil8(Tw + K - 1) x
  CI x 2 B, must fit 48 banks: half of a patch command's window.
- **Output channels**: with 16-channel output tiles (0x0c8 bit 10, as the
  patch commands) every spatial tile's input is loaded once per output
  tile: fine for 3x3 convs (compute-bound), but a 1x1 conv drops to 134
  MAC / cycle. All output channels at once: 0x074 [23:16] = 1, 0x078 = th
  x tw, 0x090 = 1 << 16 | CO, 0x0c8 = 0xf (bit 10 clear; uint8 0xc), the
  whole layer's weights at [lo, ...) (0x124 = lo << 16 | lo, below bank
  0x7e): 1x1 adds 484-489 MAC / cycle (patches 266).
- Traps: 0x0c8 bit 26 (compiled int16 commands set 0x0400000f) halves the
  outputs (one more shift); 0x0a8 = 0x54545454 (compiled) breaks everything
  here; **a tiling whose last tile is empty (th x To - Ho >= To) hangs the
  MDLA** until a reboot (mdlacmd.conv_geometry asserts it).
- Rate: output columns pad to multiples of 8, rows to 4; each tile costs
  ~0.2 us. 240 x 240 x 64 -> 32 with whole tiles: 506 MAC / cycle at
  To x Tw = 24 x 24, 40 x 24, 16 x 16 (Tw 20: 422, Tw 30: 475, 8 x 8: 479).
  x4plus at 213 px (patch rate before): c1 489 (405), c2 485 (397), c3 491
  (397), c4 490 (376), c5 495 (433), adds 469 (266); the tile 2.64 -> 2.16 s
  on one core, 490 MAC / cycle over all layers.

### DMA pool above 4 GiB (2026-11-04, night, re/e22_window.py)

The default CMA area (cma=512M, at 0x50000000) cannot grow: below 4 GiB
lk's carve-outs leave no free range much larger (4d100000-6fffffff, 559
MiB). The MDLA's addresses are 32-bit, but the reviser boundary supplies
bits 33:32:

- DT: reserved-memory apu-pool@190000000 (shared-dma-pool, reusable, 1
  GiB, inside the 4 GiB window 1) and /cezanne-apu { memory-region }.
  cezanne-apu makes it its device's CMA area
  (of_reserved_mem_device_init_by_idx), DMA mask 34 bits, boundary = the
  window (module parameter boundary, -1 = auto), refuses a buffer outside
  it, returns the low 32 bits as the buffer's address; without the node:
  the default area, boundary 0.
- Test (e22_window.py) before anything ran there: a test DT also reserved
  the pool's alias without bits 33:32 (0x90000000, no-map) and filled it
  through /dev/mem like the APU buffer, its command writing elsewhere, so
  the MDLA could only touch our memory. The conv came out exact in the
  pool, the alias untouched: command fetches and data both get the bits.
- x4plus at 599 px (840 MiB on one core) and 579 px tiles on both cores
  (750 MB) run from it, byte-identical to the low-memory runs.
- Reusable means movable pages live there, and some do not migrate: an
  hour after boot the largest buffer the pool gave was ~664 MiB (the
  default area ~496 of 512), and with the page cache filling RAM (10 MB
  of CMA free) 715 MiB failed every try (EBUSY, the same ~13 k pages
  failing each time; dropping caches did not help). A failed try still
  reclaims and migrates most of the range: 378 MiB then came at once.
  cezanne-apu tries 8 times with __GFP_NOWARN (linux-cezanne 7.3.1-8;
  without it every failure logs a CMA dump and a page allocator warning),
  cezanne-upscaled steps down 579 -> 386 -> 193 px. Not found: which
  pages refuse (no tracing in the config; kpageflags / kpagecgroup show
  ordinary anon and file pages of user services).

### Rate (2026-11-04, re/e19_rate.py, re/e20_layers.py)

Batches of one repeated conv command timed by the driver, and an x4plus
tile layer by layer:

- Compute-bound: int16 ~460 MAC / cycle per core (~90 % of a 512-wide
  array), uint8 ~800-890: the same per cycle at 350 and 700 MHz, both
  cores together as fast per core as one alone; the APU bus at 208 instead
  of 499 MHz costs ~10 %.
- The output width is padded to a multiple of 8 (24 x 27 runs at 389, 27 x
  24 at 437 MAC / cycle); rows that are no multiple of 8 cost a few %.
- Every command costs ~10 us more (~7000 MDLA cycles, the same in cycles at
  350 MHz): it shows with few output channels (CO 16: 320 MAC / cycle).
- 1x1 convs are memory-bound: 260 at 700 MHz, 285 at 350, 182 with the bus
  at 208 MHz.

Breaking it fails silently, in the first 8 x 8 output block (weights over
the input or into the top banks) or in whole regions (input too big; the
bottom-right corner first). So: lo = 0x60, hi = lo + one weight tile, and
patches as large as the rounded window allows: x4plus 46 x 62 px for 64
input channels, 30 x 62 (96), 30 x 46 (128), 22 x 46 (160), 30 x 30 (192)
(were 22 x 22 / 30 x 30). Not understood: lo between 0x40 and 0x60, and
small lo (0x20 holds ~16 banks).

The compiled camera networks hold 172 (apu_miai_preview.bin) and 110
(reiddetv2_apu_cache.bin) commands with VLM-relative addresses
(0x1d8xxxxx): re/cmdtab.py stats / dump tabulates their words. More in
vendor lib64: libaidepth_tuning.so (174), libarcsoft_dualcam_refocus.so
(140), libaibc_tuning.so (8 x 38, int16, PReLU); after each command
stream there are its constants as records (u64 id, u64 size, data); a
PReLU layer's shift table has id X, its multipliers id X - 0xe, in layer
order by ascending id.

Test: `insmod apuprobe.ko` (debugfs apuprobe/{peek,mem,mem_addr,run}),
`re/utpat.py <libmdla_ut.so> <dir>` extracts the patterns (RE reference,
not shipped), `re/utrun.py <dir>/<pattern>` runs one and compares under
the golden mask. Module builds: against the linux-cezanne build tree
(packaging/linux-cezanne/src, LOCALVERSION=-cezanne<pkgrel>).

### Depthwise and strided convs, int16 (2026-11-05, re/e23-e25)

For MiDaS small (upscaler/README.md "Depth"); all through /dev/cezanne-apu
against numpy, exact but for the 1-LSB requant ties.

- Stride 2 (standard and depthwise): 0x0c0 = s << 4 | s and the tile
  fields at stride s (re/mdla.py conv_geometry, as the vendor's compiled
  strided layers): window rows (To - 1) s + K, advance To s, end extent
  E s + pad_end. The padded input must be used up exactly: with a padded
  row / column the windows never reach (33 px with pad 0 / 1, 32 px with
  1 / 1) one output row and column came out wrong; dropping the unreached
  padding fixes it. TF "same" padding (MiDaS's explicit Pad ops) is exact.
  A standard 5 x 5 conv at stride 2 was wrong too (not needed, not
  pursued: MiDaS's 5 x 5 convs are all depthwise).
- Depthwise ("mode A" of the vendor's uint8 re-id / pose nets): the
  standard template with 0x070 bit 26, 0x01c = 1 << 16 | K (Cin per
  group 1), 0x090 = C << 16 | C, 0x0a8 = 0x33333333, 0x0bc = 0x00f00002,
  0x0c8 = 0xc (+ 3 for int16), 0x148 = 0x80, one channel tile (0x074
  Cout field 1), 0x04c = K K C 2 rounded up to 256. int16 weights
  [c / 16][tap][16] (one-weight probes: slot (c / 16) K K 16 + tap 16 +
  c % 16). K 3 / 5, stride 1 / 2, up to 1392 channels.
- An int16 depthwise command adds no bias: not from 0x054 (the uint8 test
  pattern does), not with the test pattern's 0x070 / 0x0c8 / 0x148 /
  0x0a8 / 0x0bc / 0x090 / 0x1bc words, the 0x15c type fields, 0x0fc or
  0x0cc (a long bias table showed no entry read). The runner adds it on
  the CPU (MiDaS: ~5 M values).
- Depthwise tiles: an output tile row of Tw px x C channels must stay at
  most ~4.5 K: up to 4480 ran, from 4608 on every tiling hung or wrote
  garbage (5 x 5 over 32 x 32 x 288 in 8 x 1 tiles hung), whatever the
  input window's size; the runner keeps Tw C <= 4096.
- All output channels at once (standard convs, the layer's weights in the
  CBUF): up to 512 channels ran (with up to 110 KB of weights), 576 hung
  even with 55 KB (re/e25_allco.py): above 512, 16-channel output tiles.
- After each of the ~15 timeouts in these tests the next command ran:
  the driver power-cycles the core's domain on a timeout
  (mdla_timeout_handle(), as the BSP), so a hang costs the timeout, not
  a reboot.

## References (RE only, nothing of it is shipped)

- MIUI vendor (build/vendor-unpack/fs): lib64/mt6885/libmdla_ut.so (test
  player + patterns), libneuron_adapter.so / bin/hw/android.hardware.
  neuralnetworks@1.3-service-mtk-neuron (Neuron compiler, MDLA 1.5
  backend), etc/camera/apu_miai_*.bin, reiddetv2_apu_cache.bin (compiled
  networks), cam_vpu{1,2,3}.img (VPU firmware, lk loads it).
- BSP: apusys/{power/2.0/mt6885, mdla/{2.1,mt6885}, reviser, mnoc, vpu,
  edma}.
