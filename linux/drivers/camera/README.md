# Cameras (Redmi K30 Ultra)

Sensors (vendor imgsensor): cam0 imx682sunny (main), cam1 s5k3t2sunny (pop-up
front), cam2 ov02b1bofilm (depth), cam3 s5k5e9sunny, cam4 hi1337sunny.

## Front camera: S5K3T2 (cam1) — milestone 1: sensor responds (2026-10-22)

| | |
|---|---|
| bus | i2c4 (0x11d02000, multi-channel, patch 0019), 7-bit 0x56 (vendor 8-bit 0xac) |
| chip id | reg 0x0000 = 0x3142 |
| DVDD 1.1 V | FAN53870 LDO1 (i2c7 0x35, reset GPIO134) — fan53870.ko |
| AVDD 2.8 V | GPIO148 load switch (regulator-fixed) |
| DOVDD 1.8 V | MT6359 vcamio |
| MCLK 24 MHz | CAMTG2 <- univpll_192m_d8, pin GPIO137 func 1 (CMMCLK1) |
| reset | GPIO131 (active low) |
| link | 4-lane CSI-2, binned modes 2592x1940 (plain Bayer); full res is 4-cell |

Power-up order (vendor cfg table): RST low, DVDD, AVDD, DOVDD (+1 ms),
RST high (+3 ms), MCLK (+5 ms). s5k3t2.ko does that and reads the ID.

## What streaming needs (none of it exists for MT6885 in mainline)

- CAM power domains (CAM, CAM_RAWA/B/C) and camsys clock gates.
- seninf (CSI-2 RX + mux) and a DMA path (camsv for raw dumps) — on this
  BSP these are programmed from the closed userspace HAL (libcam.*), the
  kernel side is only clocks/mmap; register sequences must come from the HAL
  binaries or MediaTek's upstream ISP7 (MT8195/MT8188) V4L2 work.
- SMI larbs + M4U IOMMU, or IOMMU bypass with buffers below 4 GiB.
- s5k3t2 mode register tables (vendor driver has them) + V4L2 subdev.
- libcamera simple pipeline + SoftISP for the raw Bayer stream.

## Snapshot & replay (in progress)

- `tools/camdump.py` (run in the Arch chroot under MIUI, as root) reads the
  front-end registers through the vendor kernel's mmap interfaces:
  /dev/seninf (seninf 0x1a004000 +0xa000, MIPI RX analog 0x11c80000
  +0x10000) and /dev/camera-isp (CAM_A/B/C 0x1a030000/50000/70000 +0x8000,
  CAMSV0-7 0x1a090000 + 0x1000 n). Read only while the camera streams
  (unpowered blocks can hang the bus); flushed per block.
- `dumps/front-*.bin`: MIUI front camera preview (2026-10-22). CAM_A TG
  (0x1f00: SEN_MODE 0x16003, VF on, grab 2592x1940) runs the preview; CAMSV
  0-7 are idle (reset values). `tools/camparse.py` loads the dumps.
- Plan: replay seninf + MIPI RX as MIUI programmed them, route the seninf
  output to a CAMSV engine instead of CAM_A, and let CAMSV DMA raw frames;
  sensor in the 2592x1940 binned mode from the vendor table.
- Register reference: ChromeOS chromeos-5.10 drivers/media/platform/
  mtk-isp/isp_50 (MT8183, ISP5.0; saved in downloads/cros-isp50). MT6885
  seninf keeps its per-receiver layout: SENINFn blocks every 0x1000, CSI2
  at +0xa00 (SENINF1_CSI2_CTL = 0xf: 4 lanes), mux at +0xd00 (SENINF1_MUX
  on, MUX_DEBUG_5 = 0x07940a20 = 1940 x 2592). The top block (0x0000-0x0100)
  and 0x0400-0x05f0 (ISP6s "cam mux" routing to CAM/CAMSV) differ and are
  decoded from the dump. MIUI uses SENINF1 for the front camera.
- Still needed for streaming: CAM power domains + camsys clocks, CAMSV
  register map (MT8183 cam regs as the starting point), DMA without the
  IOMMU (buffers below 4 GiB) or an M4U port.

### Milestone 2: frames reach seninf (2026-10-22)

- s5k3t2.ko `stream` (sysfs): vendor sensor_init + preview_setting
  (s5k3t2_regs.h, extracted from the vendor header) -> sensor frame counter
  0x0005 runs at 30 fps.
- cezanne_camsv.ko: `debug_power`, debugfs `regs` (camdump format), `replay`
  (replay_front.h = tools/gen_replay.py: MIUI value != Linux reset value,
  minus words that differ between the two MIUI snapshots: 97 MIPI RX + 34
  seninf writes; MIPI RX first, then seninf in address order), `status`.
- Result: with the replay done before sensor stream-on, SENINF1 0xadc reads
  0x0795xxxx while streaming (0x795 = 1941 lines, low half the running line
  count; 0x07950795 after stop, as in MIUI), mux counter 0xd40 cycles 0-3,
  CSI-2 interrupt status 0, mux size 2592x1940.
- Next: CAMSV DMA (mux output -> CAMSV0 instead of CAM_A via the 0x400/0x500
  routing block), buffers below 4 GiB.

### Milestone 3: raw frames through CAMSV0 DMA (2026-10-22)

First real image: `dumps/first-frame.png` (tools/raw2png.py of a 2592x1940
RAW10 frame, fixed exposure 0x0202 = 0x700, gain 0x0204 = 0x40, no AE).
Programmed from userspace: `tools/camsv-capture.sh <cammux> [pixmode]`
(via debugfs `reg` "sv|si|rx <ofs> [<val>]"), buffer from sysfs `dma_buf`
(dma_alloc_coherent, 32-bit mask), read back through debugfs `frame`.

- Routing: seninf cam mux block at seninf +0x400 has the ISP7 layout
  (mt8188 isp_7x patches, downloads/isp7x-seninf): CTRL_0-3 0x400-0x40c
  (4-bit source per cam mux, reset = identity), EN 0x410, OPT n 0x420 + 4n,
  CHK_CTL_0/1 n 0x500/0x504 + 0x10n (pixel mode / expected size).
  **Cam mux 0 = CAM_A, cam mux 3 = CAMSV0** (presumably 1-2 CAM_B/C,
  3-10 CAMSV0-7). Source 0 = seninf mux 1 (seninf +0xd00).
- Pixel mode: 1-pixel (seninf mux 0xd04[9:8] = 0, cam mux CHK_CTL_0 = 0,
  CAMSV TG SEN_MODE 0x00010001). 2-pixel gives IMGO errors (0x760 = 0x8288).
- DMA: CAMSV0 sits on SMI larb14 port 3, behind the display IOMMU, which
  comes up with MMU_EN = 1 per port (larb14 +0x380 + 4 port) and no page
  table, so writes went nowhere. Larb14 +0x38c = 0 -> physical addressing
  (python tools/peek.py over /dev/mem for now; lost on CAM power-off).
- CAMSV0 map (ISP5-style top block shifted by 0x10, confirmed in the vendor
  HAL libimageio_plat_drv.so CAMSV_TOP_CTRL::_config disassembly):
  MODULE_EN 0x10 (bit0 TG, bit2 PAK, bit3 unpack, bit4 IMGO, bit30 DB),
  FMT_SEL 0x14 (0 RAW8, 1 RAW10, 2 RAW12; YUV etc. take the unpack path),
  INT_EN 0x18 (vendor 0x31871), INT_STATUS 0x1c (read clear), SW_CTL 0x20,
  CLK_EN 0x30 (0x8005), **PAK 0x4c[4:0] (RAW8 8, RAW10 9;** 6/7 = 16-bit
  unpacked, low 8 bits only), PAK[6:5] pixel mode, 0x2d0[20:16] = 0xe.
  TG: SEN_MODE 0x130, VF_CON 0x134, GRAB_PXL/LIN 0x138/0x13c (end << 16),
  PATH_CFG 0x140, frame size seen 0x168, INTER_ST 0x16c.
  IMGO: BASE 0x420, BASE_MSB 0x424, OFST 0x428, XSIZE 0x430 (bytes - 1),
  YSIZE 0x434, STRIDE 0x438, CON 0x43c. DMA error status IMGO 0x760.
  FBC 0xa00/0xa04 not needed: without FBC the IMGO rewrites the same buffer
  every frame. DMA/format registers latch only at start (live writes to
  XSIZE/FMT/PAK do nothing).
- Output format: MediaTek packed RAW10 (little-endian bit stream, 4 px in
  5 bytes), 3240 bytes/line, stride 3248, Bayer GRBG, black level 64.
- Sensor test pattern 0x0600 (1 solid: 0x0602-0x0608 values, 2 colour bars)
  is what decoded the format.
- Gotchas: IMGO errors are level interrupts -> the first try IRQ-stormed the
  phone (handler now masks error bits); bring-up script must not stop VF.
- Next (step 4): move the sequence into the driver (larb14 port, cam mux,
  CAMSV setup, per-frame buffer hand-off), V4L2 capture device, libcamera
  SoftISP; add the front sensor to cezanne-popupd CAMERA_DEVICES.

### Milestone 4: V4L2 + libcamera (2026-10-22)

`cam -c1` gives processed frames (dumps/libcamera-frame.png: software ISP
on the GPU, AE + AWB, 30 fps); `v4l2-ctl --stream-mmap` raw SGRBG8 at 30 fps.
The pop-up rises on stream-on and retracts after stream-off.

- s5k3t2.ko: V4L2 subdev (/dev/v4l-subdev0), one mode SGRBG10_1X10
  2592x1940; controls PIXEL_RATE 678.26 MHz, LINK_FREQ 267 MHz, HBLANK 8288
  (line 10880), VBLANK >= 138 (frame 2078 -> 0x0340), EXPOSURE 4..frame-8
  lines (0x0202), ANALOGUE_GAIN 0x20..0x200 = 1x..16x (0x0204), TEST_PATTERN
  (0x0600), orientation from DT. Powers up on s_stream.
- cezanne_camsv.ko: MC-centric capture /dev/video0 (SGRBG8 2592x1940, stride
  2592), media device driver name **"mtk-seninf"** (libcamera's simple
  pipeline handler matches on it), sensor bound through the DT graph
  (port/endpoint on both nodes). vb2 dma-contig from CMA below 4 GiB
  (cma=128M on the command line). Start: larb14 port 3 MMU off, seninf/MIPI
  replay, cam mux 3, CAMSV RAW8 + FBC, first buffer, VF on, sensor on.
  Buffers: IMGO address programmed only after the previous one latched (SOF
  IRQ), completions on FBC write count (CTL2[15:8]). sysfs `streaming` for
  cezanne-popupd (it polls it every 0.2 s when down).
- RAW8, not RAW10: CAMSV's unpacked mode gives 10-bit << 4 and packed RAW10
  is MediaTek's bit order; libcamera's software ISP reads neither.
- Phone setup (also in mainline/rootfs): media core modules (mc, videodev,
  v4l2-async, v4l2-fwnode, videobuf2-{common,v4l2,memops,dma-contig}) and
  dma-buf heaps (system_heap, cma_heap; modules-load.d/cezanne-camera.conf)
  copied by hand into /lib/modules/7.2.11-dirty/kernel; udev rule
  62-cezanne-dma-heap.rules (uaccess); /etc/libcamera/configuration.yaml
  (mtk-seninf: software_isp true, GPU mode); tuning file
  /etc/libcamera/ipa/simple/s5k3t2.yaml (black level 4096 on the 16-bit
  scale; without it the image looks washed out).
- Known: "MESA: error: WSI pitch not properly aligned" from the GPU debayer
  (frames are fine); no CameraSensorHelper for s5k3t2 (linear gain code
  assumed, which is right: 32 per 1x); sensor rotation not set yet (check
  in an app); v4l2-ctl reports sequence gaps (frames dropped while no
  buffer is queued).

### Milestone 5: apps — rotation, colour, Snapshot fork (2026-10-22)

Front camera usable in GNOME: upright portrait preview (1940x2588), AE/AWB,
photos. Overall colours judged good at normal distance.

- Rotation: the sensor is mounted sideways; DT `rotation = <90>` (V4L2
  counter-clockwise correction; 270 gave an upside-down preview). CAMSV and
  the sensor cannot rotate, so libcamera's software ISP does it:
  apps/libcamera-softisp-rotation.patch (libcamera v0.7.2, built and installed
  on the phone over the ALARM package from /home/arch/src/build-libcamera —
  a pacman upgrade of libcamera reverts it):
  - simple pipeline validate(): what the sensor cannot do (transpose) goes
    to the software ISP as ispTransform for processed streams, orientation
    reported as requested; processed sizes checked/adjusted in sensor
    orientation; generateConfiguration() lists transposed processed sizes.
  - Debayer::supportsTransform()/setTransform(); DebayerEGL supports any
    transform by permuting the texture coordinates of the output quad (no
    shader change), viewport transposed. (Bugs met: client-side vertex array
    must outlive the draw call; viewport must follow the transposed size.)
    The CPU debayer keeps Identity only.
  - Also fixed: simple pipeline counted streams from the built-in table
    before the configuration-file software_isp override, so the raw stream
    next to a processed one did not exist ("generateConfiguration() returned
    nullptr" in libcamerasrc).
  - Verified with the sensor colour-bar pattern (white..black left->right
    natively, top->bottom after Rot90).
- Colour: grey-world AWB + tuning CCM in /etc/libcamera/ipa/simple/s5k3t2.yaml
  (row sums 1.02/0.98/0.96: identity looked bluish, 1.04/1.00/0.92 green).
  Not a calibration.
- Lens shading: close, evenly lit white paper shows a bright centre and green
  corners; the software ISP has no LSC. Fine at normal distances.
- Snapshot fork (dev tree apps/snapshot; packaged since 2026-11-02 as the
  snapshot overlay, packaging/overlays/snapshot: 0001-cezanne-fork.patch made
  by its update-patch.sh, app id org.gnome.Snapshot in place of Arch's; the
  helpers below are the package cezanne-photo, mainline/camera/hdr):
  libcamera backend (libcamerasrc +
  libcameraprovider, no portal), settings sheet built from libcamerasrc
  control specs, RAW (DNG) toggle, gallery hides .dng, libcamera cameras use
  their largest (full field of view) size — smaller sizes are crops.
  libcamera device provider: GstDevice "name" is shadowed (use
  display-name), location is api.libcamera.Location (int).

Open:
- RAW/DNG from Snapshot not verified yet: the raw stream now runs next to the
  preview (`cam -s role=viewfinder -s role=raw` works, gst-launch with
  src_0::stream-role=raw delivers 5 MB SGRBG8 buffers) but no photo with RAW
  on has been taken since; aperture logs (RUST_LOG=aperture=debug) will tell.
- Snapshot crashes (SIGSEGV) in GTK's GL renderer
  (gsk_renderer_render_texture from ShutterButton::draw_border) on the second
  photo; the photo itself is saved. GPU/driver side.
- Full-resolution preview drives the display pipe into continuous stalls
  (2068 kick-driver recoveries in one session) -> display/GPU task.
- Later goal: colour calibration (CCM per colour temperature, lens shading
  correction in the software ISP, per-unit calibration from MIUI's data).

## Main camera: IMX682 (cam0) — streaming 2026-10-25

`cam -c <back>` gives processed 4624x3472 frames at 30 fps (software ISP,
rotated to portrait); raw SRGGB8 on /dev/video1. dumps/main-first-frame.png
(raw), dumps/main-libcamera.png (processed).

| | |
|---|---|
| bus | i2c2 (0x11d01000, SPI 114, multi-channel), 7-bit 0x1a; chip id 0x0016 = 0x0682 |
| AVDD 2.9 V | FAN53870 L3 (vendor table calls it "DVDD") |
| AVDD2 1.8 V | FAN53870 L5 |
| DVDD 1.1 V | GPIO133 load switch (vendor "PDN") |
| DOVDD | MT6359 vcamio |
| MCLK 24 MHz | CAMTG (TOP_MUX_CAMTG) <- univpll_192m_d8, GPIO149 func 1 |
| reset | GPIO145 (active low) |
| link | 3-trio C-PHY on CSI3 -> SENINF7 |

Rails/GPIOs read back in MIUI while streaming (/sys/class/regulator,
/sys/devices/platform/pinctrl/mt_gpio). Vendor pinctrl state names are
indexed by pinctrl-N, decode them by name (cam0_rst = GPIO145, cam0_pnd =
GPIO133, cam0_ldo_vcama = GPIO164 unused). Ultrawide (cam4, HI1337): reset
GPIO144, pnd GPIO134, L2 1.1 V + L4 2.8 V, MCLK GPIO151, i2c4, CSI2 D-PHY 4
lanes -> SENINF5 (dumps/uw-on*.bin). Depth/macro on i2c8 (not done).

- imx682.ko: vendor sensor_init + preview tables (tools/extract_imx682.py,
  DPHY_2LANE = 0 branch), one mode 4624x3472 (2x2 binned, full FOV), pixel
  clock 1029 MHz, line 9432, frame 3636; gain 0x0204 = 1024 - 1024/gain
  (Sony, up to 64x), group hold 0x0104. No QSC/LRC/PDAF.
- replay_main.h: tools/gen_replay.py -n main -m c000-10000 -s
  006c-0070,1d08-1e00,6000-7000,8000-a000 (CSI3 analog, top 0x06c = 2 =
  C-PHY, SENINF7, mux 2, cam SMI sub-commons); MIUI dumps main-on*.bin /
  uw-on*.bin (main streams in both).
- cezanne_camsv.ko is now per-channel (port 0 front, port 1 main), each
  with its own video node; both use CAMSV0 (cam mux 3, larb14 port 3), one
  stream at a time (second STREAMON gets EBUSY).
- **larb13**: CAMSV1-6 DMA goes through SMI larb13 -> MDP SMI common
  (0x1f002000) -> MDP IOMMU (0x1f027000). Mainline configures none of it:
  CAMSV1 FBC drops every frame (RCNT_INC ignored), CAMSV2 "writes"
  (checksums at 0x9c0 move) but nothing reaches RAM and larb13 STAT stays
  busy. Not bus protection (MM EN 0x2d0 only has the CAM bits; STA1 bits are
  acks). Cam sub-common 2 (0x1a00e000) was also unconfigured; configuring
  it did not help. Needed for concurrent cameras only.
- libcamera: apps/libcamera-cezanne-sensors.patch (helpers + static
  properties for imx682 and s5k3t2), tuning /etc/libcamera/ipa/simple/
  imx682.yaml (black level 4096). DT rotation 270 (raw is upright with the
  phone right-edge-up). CMA 512 MB for the CAMSV buffers; CMA pages serve
  movable allocations while the camera is idle.

### Focus, capture path, colour (2026-10-25)

- Lens: DW9800K at i2c2 0x0c, VDD FAN53870 L7 2.804 V (8 mV grid: 2.8 V
  fails), mainline dw9719.ko (compatible dongwoon,dw9800k).
- AF: contrast detection in the soft IPA (apps/libcamera-softisp-af.patch):
  16x12 sharpness grid every frame, coarse then fine scan, continuous by
  default, AfWindows for tap-to-focus. A lens move needs 4-6 frames.
- CAMSV buffers are cached (vb2 non_coherent_mem, set in queue_setup):
  uncached reads made the CPU stats 6x slower (debayer+stats 44 -> 5 ms).
- Preview runs at 1080x1436 (the EGL debayer scales the whole field);
  Snapshot switches the caps to full size for a capture and back
  (libcamerasrc renegotiates, the IPA keeps AE/AWB/focus over the same-mode
  reconfigure, the first 3 frames after the restart are skipped: black).
- Soft ISP output buffers come from the system dma-heap: CMA fragmented
  (16 MB allocations failed with -EBUSY and 266 MB free).
- Stats window: the EGL debayer measured an output-sized top-left corner;
  now the whole native area (4th line pair only on large frames).
- AGC: highlight protection from a count of raw samples with a saturated
  channel (>2 % clipped lowers exposure down to MSV 1.4). The exposure that
  clipped becomes a ceiling (stay 7 % under it until clipping drops below
  0.1 %): a bright window went 0.4 % <-> 3.5 % clipped per 4 % step and the
  preview pumped for ~10 s.
- Tap to focus in Snapshot: the controls overlay (AdwMultiLayoutView) covers
  the viewfinder, so taps never reached its gesture. The camera page
  catches presses in the capture phase, skips controls and forwards the
  point to the viewfinder. A tap also switches AF back to continuous.
- Switching cameras crashed or hung (heap corruption), found with an ASan
  build of libcamera (/home/arch/src/build-lc-asan -> /opt/lc-asan,
  /tmp/asantest.sh runs Snapshot on it). Causes, all in libcamera:
  - libcamerasrc never disconnected Camera::requestCompleted on close:
    reopening a camera (switching back) added a second handler, every
    completion was handled twice - freed by the first, used by the second.
  - libcamerasrc ignored queueRequest() failures and paired completions with
    the front of its queue blindly (now: only queued on success, matched by
    pointer).
  - simple pipeline: a cancelled internal (request-less) capture buffer
    with a raw stream dereferenced a null request; a request completed while
    still in conversionQueue_ was cancelled again on stop.
  - IPA: stats without valid sensor controls complete the frame without the
    algorithms instead of asserting.
  Snapshot exports app.switch-camera for scripts (/tmp/swtest.sh).
- Tap to focus is off for the front camera (fixed focus) and AF mode is
  only set on the main camera.
- libcamerasrc applied pending controls to a new request before checking
  for free buffers; a request dropped with -ENOBUFS (routine while
  streaming) took them along, so a tap's AfWindows often never reached the
  IPA. Controls are now applied right before Camera::queueRequest().
- Colour fitted to MIUI shots of the same scene from the tripod
  (backup/camera-ref: MIUI JPEGs, raw/processed Arch frames). MIUI renders
  the white keys neutral; AWB is a tile vote along a white locus (tuning
  `locus`) blended with grey world over unsaturated tiles (the old grey
  world counted clipped window pixels: the green/cyan cast). CCM fitted on
  uniform blocks (rows sum to 1, MIUI saturation x1.6 folded in), Adjust
  gamma 3.0 / contrast 1.1 (new tuning keys) for MIUI's lifted midtones.
  Front: wall-only scene, locus anchored on it, main CCM at x1.3; strong
  lens shading (no LSC in the soft ISP).
- Second scene (backup/camera-ref/scene2): the fitted matrix turned reds
  orange. MIUI's Pro mode saves DNGs with Xiaomi's calibration
  (ColorMatrix/ForwardMatrix for D65 and Standard A, AsShotNeutral, a
  lens-shading GainMap in OpcodeList2): imx682.yaml now uses ForwardMatrix
  -> linear sRGB at both illuminants (saturation x1.4) and the calibrated
  neutrals for the tungsten end of the locus. MIUI's own white for daylit
  scenes renders white paper bluish; ours keeps it neutral.
- Tap to focus: the IPA refocuses only when the window changes, with a
  local hill climb from the current lens position (~1.3 s instead of a 2.2 s
  full sweep). Snapshot exports app.focus-at((dd)) to test it from a script.
- apps/libcamera-softisp-capture.patch has all of the above on top of the
  AF patch.

### Flash LED (2026-10-25)

- MT6360 FLED1/FLED2 (mainline leds-mt6360, CONFIG_LEDS_MT6360=m, DT
  led-controller under the MT6360): white:flash-1 / white:flash-2, torch up
  to 200 mA (15 steps), strobe 750 / 800 mA (stock values). Three LEDs on
  two channels: one channel lights two of them. Found by measuring the rear
  scene with each output lit (ISINK1-4 do not reach it); the pop-up
  notification LED did not react to ISINK1-4 either (stays red when
  charging): driven by something else, not done.
- udev 64-cezanne-flash.rules: video group may write brightness / flash_*.
- Snapshot: flash button Off / Auto / Torch (gsettings flash-mode). Torch:
  LEDs at half power while the rear camera streams on a visible page. Auto:
  for pictures in low light (exposure >= 30 ms and gain >= 4) the LEDs go to
  full torch power, the picture waits until the exposure settles (polled,
  max 2.5 s). No strobe sync. A SIGTERM/SIGINT handler switches the LEDs
  off. AGC steps grow with the error (max 30 %) so the exposure follows the
  light switching on.
- GNOME: rootfs/camera/cezanne-flashlight@arch-port, a Quick Settings
  toggle with Low / Medium / High.

## All rear cameras and concurrent capture (2026-10-25)

Sensor names: cam2 "depth" OV02B1B is a **monochrome** 2 MP sensor (vendor
`SENSOR_OUTPUT_FORMAT_RAW_MONO`); cam3 S5K5E9 is the 5 MP macro (with AF);
cam4 HI1337 the 13 MP ultrawide.

### Second DMA path: CAMSV2/3 through the MDP IOMMU

- CAMSV1-6 write through SMI larb13 -> MDP SMI common (0x1f002000) -> MDP
  IOMMU (BSP iommu1, 0x1f027000). That IOMMU translates larb13 traffic
  **even with the larb's MMU bits clear**: with its fault interrupt
  enabled, CAMSV writes faulted at their physical address. So larb13 is a
  real IOMMU client now (patch 0042: mdpsys SMI clock gates; DT: mdpsys,
  smi_mdp, larb13, iommu_mdp as a second mt6893 IOMMU instance sharing
  iommu_mm's page table).
- The larb's bank select does not reach this IOMMU: a CAMSV write to IOVA
  0x2_fe000000 arrived as 0x0_fe000000 and landed in a display buffer.
  Patch 0043 puts larb13 ports 0-8 in IOVA region 0 (below 4 GiB).
- Port mapping (bank-select probing: port banked to 1 -> its faults move
  to 0x1_xxxxxxxx): CAMSV2 and CAMSV3 IMGO both arrive on larb13 port 6,
  so the BSP port names do not map 1:1; the camera node lists ports 3-8.
- CAMSV1 ignores RCNT_INC (FBC drops every frame): unused.
- Each CAMSV writes something per frame to the address in **0x804** (frame
  header?): 0 there = an IOMMU fault per frame. It gets a scratch buffer
  (found by bisecting which register silenced the faults).
- The first buffer completed after stream-on is never written (stale
  memory with IOMMU buffers: garbage flashed on camera switches and
  captures): dropped and requeued.
- cezanne_camsv.ko: 5 channels (front, main, uw, macro, mono); with the
  IOMMU all use CAMSV2 except mono on CAMSV3 (`engines=` overrides), so
  main or ultrawide stream together with the mono sensor (HDR). Buffers
  are IOMMU-mapped (no CMA needed). Video nodes are named
  `cezanne-<channel>`.
- Replays must not touch shared registers: the front table (from a full
  MIUI dump) wrote CSI3's analog block and CAM_MUX_EN (disabling another
  channel's cam mux); regenerated with ranges (gen_replay -m/-s).

### Sensors (cezanne_sensor.ko, one table-driven driver)

| | ultrawide HI1337 | macro S5K5E9 | mono OV02B1B |
|---|---|---|---|
| bus | i2c4 0x20 | i2c8 0x2d | i2c8 0x3c |
| MCLK (measured) | GPIO151, CAMTG3 | GPIO136, CAMTG4 | GPIO150, CAMTG2 (shared with front) |
| reset | GPIO144 | GPIO130 | GPIO143 |
| rails | FAN53870 L4 2.8 V, L2 1.1 V | GPIO148 2.8 V (front's), GPIO146 1.2 V | GPIO147 2.8 V, GPIO16 "pwdn" |
| link | CSI2 D-PHY 4 lanes -> SENINF5 | CSI1 2 lanes -> SENINF4 | CSI1 1 lane -> SENINF3 |
| mode | 4208x3120 GRBG10, 30 fps | 2592x1940 GRBG10, 30 fps | 1600x1200 Y10, 30 fps |

- Tables: tools/extract_aux_sensors.py -> cezanne_sensor_regs.h (HI1337
  init is a firmware upload through a data port: single writes).
- MCLK: the BSP sensor index does not give the CAMTG (UW: TG3 not TG5;
  mono answered on TG3 at boot only because the front's TG2 happened to
  run): found by probing each CAMTG.
- OV02B1B drops exposure/gain written before stream-on (controls after).
- Macro and mono settings from MIUI dumps (tools/miui-camdump.sh: macro
  mode, portrait mode = main + mono); both on CSI1 with different lane
  mappings in its digital block: not both at once.
- i2c8 (0x11d04000) added to the DT. Rotation 270 for all rear sensors.
- Macro VCM: DW9800 (IC info 0xf2) at i2c8 0x0c, FAN53870 L6; vendor mode
  SAC2 / prescale 0x7f; mainline dw9719 (dongwoon,dw9800k).

### libcamera (apps/libcamera-cezanne-all.patch: everything on v0.7.2)

- Helpers and properties: hi1337 (gain (code+16)/16), s5k5e9 (code/32),
  ov02b1b (code/16), black level 64@10 bit; tuning hi1337.yaml /
  s5k5e9.yaml (calibrated later: see "Colour of the ultrawide and macro").
- Soft-ISP AF, found with the macro in a dim room: band-pass statistic
  (2x2 same-colour blocks, floor on the sums for sub-LSB steps), the whole
  frame when the window is featureless, and the **AGC held during a scan**
  (it raised the gain with the lens position: noise climbed with it and
  the scan ended at the far end).
- Debayer: colour stripes in downscaled previews (Bayer site aliasing,
  beat 1/(1-frac(scale/2)) px). Fixed in Snapshot by a preview size that
  does not make the soft ISP stretch (round up: 1456 not 1454); the shader
  also snaps downscaled samples to one quad site (exact texel centres -
  a tex_step of 1/(size-1) crossed two texels near edges: single lines).
- The mono camera: simple pipeline cannot configure Y10; it is read by
  cezanne-mono over V4L2 instead.

### Snapshot (packaging/overlays/snapshot/0001-cezanne-fork.patch)

- Lens selector (0.6 / 1x / macro) for the rear cameras, by sensor model;
  the switch button flips front/rear (back to the last rear lens).
  Mono cameras (ColorFilterArrangement MONO) are not listed.
- Layout (phone upright): flash, 64MP, portrait, HDR top left; settings,
  menu top right; camera switch bottom left; gallery + shutter centre (the
  timer button is gone since 2026-10-29; 64MP was gone from then until
  2026-11-04).
- Format JPEG / PNG (pngenc; the PNG choice used to only rename a JPEG)
  and JPEG quality in the settings sheet.
- Post-processing, in the background, in place (`hdr.rs`, helpers in
  hdr/): noise reduction (Off / Auto by gain / Low / Medium / High), HDR,
  AI upscaling. Spinners on the gallery button and the picture while
  processing; a marker file per job (with the helper's pid) keeps that
  across app restarts; the helper finishes when the app closes.
- **HDR** (main + ultrawide): cezanne-mono streams the mono sensor while
  HDR is on and takes a bracket (EV -2 / 0 / +1.5) with each picture;
  cezanne-photo merges it to radiance, registers it to the picture (ORB +
  homography, ~250-700 inliers), takes the mono data in clipped highlights
  and deep shadows, tone maps the base layer (midtones unchanged) and
  applies a smooth gain map in linear light. ~2 s.
- **Noise reduction**: NLM luma (h 2 / 3 / 4.5) with 35 / 30 / 25 % of the
  original blended back, chroma at quarter resolution. Keeps 93 / 78 /
  48 % of the high-frequency energy (full-strength NLM kept 68 / 42 / 22 %
  and made surfaces waxy). Auto: gain < 2 off, < 5 low, < 12 medium.
- **AI upscaling**: only with the "64MP" toggle (gsetting upscale, off by
  default; photo mode, shown when an upscaler is installed; the settings
  sheet only picks its factor, 2x / 4x, and the button then reads 64MP /
  256MP). Since 2026-11-04 on the APU: Real-ESRGAN x4plus (Upscayl's
  standard model) in int16 on both MDLA cores through cezanne-upscaled
  (package cezanne-upscale, mainline/apu/upscaler/README.md), float
  quality, ~7.5 min per 16 MP picture at 2x. Without it (service missing or failing) on the GPU:
  Real-ESRGAN ncnn on Vulkan (PanVK), Upscayl's compact upscayl-lite-4x
  (2x = 4x reduced by Lanczos). 512x512 crop: 9 s (lite) vs 139 s
  (realesrgan-x4plus): ~10 min per 16 MP picture vs ~2 h.

## Image quality and the mono camera's features (2026-10-25, evening)

Everything here is done; the camera app is complete for now.

### Lens corrections (libcamera "Lens" algorithm, GPU debayer)

- **Shading** (vignetting, colour shading) from each module's own factory
  calibration: EEPROM (main 0x50 on i2c-3, ultrawide 0x51 and macro 0x52 on
  i2c-4/5; dumps/eeprom, read with `eedump` while the sensor streams) at
  0xcc: a 15x15 grid, per point R, Gr, Gb, B as big-endian u16, 8192 = 1.0
  (1.0 in the centre; corners up to 3.9x on the ultrawide, 3.4x main, 1.9x
  macro). tools/lens_tuning.py writes the tuning block. The shader applies
  it after the black level as an RGBA8 texture (gain = 1 + 3 x texel).
  The EEPROM also holds the factory AWB point (0x66: R, Gr, Gb, B under
  ~5000 K); nothing for distortion (8 KB, mirrored to 16 KB).
- **Ultrawide distortion**: the shader maps every output pixel to its raw
  position, raw = c + (u - c)(1 + k1 s + k2 s^2 + k3 s^3 + k4 s^4) with
  s = |u - c|^2 / R^2 (R the half diagonal), demosaicing at the 4 raw
  pixels around it and blending. Calibrated from 5 checkerboard pictures
  (tools/checkerboard-19x11.png full screen on the TV, DNGs in
  dumps/calibration/uw-checkerboard; tools/calibrate_distortion.py):
  partial grids detected (findChessboardCornersSBWithMeta), OpenCV's
  rational model to 1.09 px RMS, row/column straightness 7.2 px -> 0.44 px
  (34.9 -> 0.96 max); the 4-term polynomial follows it to 0.56 px mean
  (7 px in the extreme corners; 3 terms: 75 px). Corners lose ~10 % of
  the field of view. No measurable preview slowdown.
- Dead ends kept for reference: fitting against MIUI's own 0.6x picture
  (its correction is weak: the fit made lines worse, 14.8 px); plumb-line
  fits on one room picture (degenerate: scale, runaway centre, sagging
  blinds); MIUI's LDC table (/vendor/etc/camera/LDC_packdata_xiaomi_J7.dat,
  544 bytes) is encrypted (Altek libalLDC.so). The dual-camera "golden"
  files com.xiaomi.dcal.{wu,wt}.golden are plain doubles XOR 0xaa
  (dumps/vendor): main + ultrawide and main + mono intrinsics/extrinsics;
  their ultrawide distortion only half straightened the lines.

### Colour of the ultrawide and macro

- Matched to the main camera on a colour chart shown on the LG C2
  (tools/colour_chart.py: `chart` makes 48 patches + ArUco markers,
  `sample` reads the patches from a DNG with the shading applied - the
  blurry macro picture located by the screen outline -, `fit` writes the
  tuning; data in dumps/calibration/colour-chart).
- The white locus: the main camera's, scaled per channel by this module's
  grey R/G, B/G over the main's, measured twice (factory light from the
  EEPROMs ~5000 K, the display's white ~9000 K) and interpolated. Matrices:
  the main's, after a 3x3 (rows sum to 1) taking this module's white
  balanced patches to the main's (patch error 9.8 -> 8.8 % UW, 7.2 -> 6.5 %
  macro: the colour responses barely differ, the white balance did). Tone
  (gamma 3.2, contrast 1.15) as the main camera.

### 10-bit raw

- CAMSV writes RAW10 as 16 bits per pixel, LSB aligned: FMT_SEL 1, PAK 13
  (PAK 6/7: MSB aligned; 12: 8-bit in 16; 9: MediaTek's packed RAW10, 4
  pixels in 5 bytes as one little-endian bit stream, which nothing reads).
  V4L2 SGRBG10 / SRGGB10 / Y10, offered before the 8-bit formats
  (`raw10=0` hides them). libcamera picks them by itself: the GPU debayer
  already read 16-bit Bayer (the unpacked formats share the CSI2P cases:
  same fourcc), the CPU statistics too; added: AF and AWB grid statistics
  at 10 bits (scaled to the 8-bit range the IPA expects). DNGs are 16-bit
  with black 64 / white 1023. Still 30 fps.
- cezanne-mono reads Y10 and saves 16-bit PGMs (maxval 1023, black 64);
  cezanne-photo reads 8- or 16-bit PGMs.

### Autofocus fixes

- The scan's first sample (at the start position, right after stream
  start or a capture reconfiguration) could still show the lens where it
  was before: once it won with 2.6x every later sample and the lens
  "focused" at 0. The first position is measured twice.
- The fine scan counted the climb towards the coarse peak as drops and
  stopped before it: drops count only past the peak (sharpness 179k ->
  298k on the test scene).

### Portrait (main + mono stereo depth)

- Since 2026-11-04 the subject comes first from the APU's salient object
  mask (cezanne-upscaled's segment job, U^2-Net since 2026-11-05, before
  U^2-Net small; on the picture turned upright by its Exif orientation, as
  the network knows upright scenes: portraitApu(), the mask snapped to the
  picture's edges, one blur strength; ~15 s with PNG files instead of
  minutes) and Snapshot shows the toggles on every camera: two since
  2026-11-05, objects (lightbulb: the salient object, gsetting portrait)
  and people (person: U^2-Net's human segmentation, portrait-people,
  `cezanne-photo --subject people`), one at a time; the blur graded by
  the APU's depth map (MiDaS small, the `depth` job) since 2026-11-05;
  the stereo depth below is the fallback when the mask finds no subject
  (main camera, with the bracket; CEZANNE_PORTRAIT=stereo forces it).
  mainline/apu/upscaler/README.md "Segmentation".
- Snapshot: Portrait toggle (person icon) next to HDR, main camera only;
  the mono stream runs and the bracket's EV 0 frame is used
  (`cezanne-photo --portrait <prefix>`, before HDR; ~25 s).
- Stereo (stereoPair(), shared with low light): the picture turned to the
  sensors' orientation (of the two quarter turns, the one whose SIFT
  matches agree on a similarity without rotation - SIFT itself matches
  either way), at the mono focal length (golden files: main 3466 px,
  mono 1124 px), matched in **linear light** (SGBM did not survive the
  picture's gamma against the linear mono frame), slightly smoothed
  (1.5 px: low light). Uncalibrated rectification (fundamental matrix,
  Hartley): the golden intrinsics left ~1 px of row error on processed
  pictures, the fitted geometry 0.3 px. The main camera is the right-hand
  view in sensor orientation (Hartley's homographies shift x arbitrarily:
  the sign of the matches tells nothing). SGBM 3-way, search range from
  the matches.
- Subject: focus disparity = peak of the central disparities weighted by
  sharpness (the lens focused on the subject); tolerance 10 % of the step
  from the far background (10th percentile), background beyond 20 %.
  Three cues: stereo labels spread edge-aware (normalised guided filters;
  a quarter of the pixels labelled for full confidence - stray matches on
  blinds, empty areas outside the mono view; guided outputs clamped at 0,
  their overshoot made the ratio blow up), the picture's own focus (fine
  over coarse detail energy on the gamma image ~ square root of linear,
  noise-relative thresholds: high only at sharp edges, whatever their
  contrast; it overrides stereo where reflections on glossy glass show the
  room's depth), GrabCut seeded with the confident labels (restricted to
  a band around the evidence: its colour model once took the white table
  for the cream label). Then only the regions the focus seed lies on,
  notches up to 10 % of the frame closed, holes filled, edges refined on
  the full picture with a guided filter.
- Blur: disc kernels at a quarter of the resolution in 6 levels, radius
  by the disparity difference (proportional to a real lens's circle of
  confusion), normalised over background pixels only (no subject bleed).
- Debug: `CEZANNE_PHOTO_DEBUG=<dir>` writes the intermediate maps.
- Test pictures: dumps/calibration/portrait (oil bottle: glossy dark
  glass, the hardest case for every cue).

### Low light (mono-guided noise reduction)

- Part of noise reduction: with Denoise on (auto by gain: >= 2x low,
  >= 5x medium, >= 12x strong) on the main camera, the mono stream runs
  and each picture takes a bracket; `cezanne-photo --denoise N --lowlight
  <prefix>` (~10-20 s). The plain denoiser where the frames do not match.
- The mono frame warped onto the picture pixel by pixel through the
  stereo disparity (filled where unmatched; a homography would leave
  parallax ghosts), scaled to the luma's level and gamma encoded; luma =
  guided filter of the picture's luma with it as the guide (its clean
  structure, the picture's local brightness and contrast - which absorbs
  the different colour response) plus 40-20 % of the picture's own fine
  detail; chroma guided by it at a quarter of the resolution; where
  band-pass correlation is low, the plain denoiser.
- Dark room (dumps/calibration/lowlight-dark: main at max gain, luma noise
  14 levels; mono gain 15.5x): far cleaner than the NLM denoiser (which
  stays grainy with colour blotches), structure kept, a little soft
  (the mono is 2 MP); darkest areas slightly lifted. Moderate light
  (dumps/calibration/lowlight, noise 2 levels): about the same as NLM.
- Bug found on the way: the mono frame was scaled to 8 bits by its
  maximum - at high gain one hot pixel made it nearly black and the match
  count fell from 375 to 144 (99.5th percentile now).

### Tools (mainline/camera/tools)

lens_tuning.py (EEPROM shading -> tuning), calibrate_distortion.py
(checkerboard DNGs -> distortion), checkerboard-19x11.png, colour_chart.py
+ colour-chart.png (chart, sampling, camera-to-camera colour fit).
