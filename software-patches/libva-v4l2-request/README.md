# cezanne VA-API driver

VA-API (libva 2.24 / VA-API 1.24) on top of the cezanne-vdec V4L2
stateless decoder (mainline/vcodec/cezanne-vdec) and the cezanne-venc V4L2
stateful H.264 encoder (mainline/vcodec/cezanne-venc). It is a fork of
Bootlin's libva-v4l2-request: the object heap and the overall shape are
kept, and the rest is rewritten. Bootlin's sources and build files are in
`old/`.

- Decoding (VLD): H.264, VP8, VP9 (profiles 0 and 2), AV1, HEVC Main /
  Main10. Output NV12 or P010.
- Encoding (EncSlice): H.264 constrained baseline / main / high from NV12
  or RGB (BGRX, BGRA, RGBX, RGBA; the hardware converts to BT.709).
- Video processing (VAProfileNone / VideoProc): copies, RGB <-> NV12 and
  scaling on the CPU, no filters.

## Build and install

`./deploy.sh` (from the host) copies `meson.build` and `src/` to the phone,
builds them as `arch` in `/home/arch/build/cezanne-va`, and installs
`/usr/lib/dri/cezanne_drv_video.so` with the link
`panfrost_drv_video.so -> cezanne_drv_video.so`. libva picks a driver by
the DRM driver name of the render node, and on this phone that node is
panfrost. No `LIBVA_DRIVER_NAME` is needed.

The kernel module has to be loaded. At boot `cezanne-vdec.service` loads
it late; by hand use `mainline/vcodec/test/vdec-load.sh`.

## Clients

- Firefox (RDD process, sandboxed): works with the forced hardware
  decoding pref.
- mpv: `/etc/mpv/mpv.conf` sets `hwdec=auto-safe`.
- GStreamer (`gst-plugin-va`): `/etc/environment.d/60-cezanne-video.conf`
  sets `GST_VA_ALL_DRIVERS=1`, because gst only loads allowlisted vendors
  otherwise. It also ranks `vah264dec` / `vavp9dec` above v4l2codecs.
  `vah264enc` and `vapostproc` need the patched va plugin
  (mainline/rootfs/gstreamer, patches 0003 / 0004).
- GNOME's screen recorder (Ctrl+Shift+Alt+R / the screenshot UI): its
  `hwenc-dmabuf-h264-vaapi` pipeline, zero copy, about 1.5 % of one core
  for 1080x2400 against about 65 % with openh264. A pipeline that failed
  once is skipped until logout
  (`/run/user/1002/gnome-shell-screencast-pipeline-blocklist`).
- ffmpeg: `-hwaccel vaapi -hwaccel_device /dev/dri/renderD128`.

## Design

- **Surfaces.** Every VA surface is a linear NV12 dma-buf, so clients can
  export it (`vaExportSurfaceHandle`) and import it into EGL. Surfaces come
  from `/dev/dma_heap/system`, or from a panfrost BO exported with PRIME on
  the VA display's DRM fd. Firefox's sandbox denies the heap but allows
  `/dev/dri`.
- **Decoding** is synchronous in `vaEndPicture`. With one request in
  flight, `cvd_submit` does the following:
  1. `S_EXT_CTRLS` with the request;
  2. QBUF the capture buffer, then the bitstream buffer, with the timestamp
     set to a counter;
  3. queue the request and poll it;
  4. DQBUF both buffers and reinit the request;
  5. detile the MM21 capture buffer into the surface.
- **H.264.** SPS, PPS, scaling lists and decode params are built from the
  VA buffers. `pps_id` is parsed from the slice header.
- **VP9.** The driver parses the uncompressed header itself, because VA
  lacks the loop-filter deltas, the quantizer index and the segmentation
  data. The loop-filter and segmentation state persists across frames.
  The compressed header is not needed: the kernel driver reads the
  hardware's post-delta probabilities back.
- **Encoding** (`encode.c`): VA's stateless encode model on a stateful
  encoder. The kernel driver keeps the reference and the reconstruction,
  runs the rate control and writes SPS / PPS before every IDR, so the
  config offers no packed headers, one slice, one list-0 reference and no
  B frames. Sequence / picture / misc parameters become V4L2 controls
  (profile, level, entropy coding, CQP / CBR / VBR, bitrate, QP range,
  frame rate); the kernel GOP is off and an IDR picture forces a key
  frame. Reconstructed and reference surfaces are not used. The input
  surface's dma-buf is queued to the OUTPUT queue as it is (an OUTPUT
  index per surface keeps vb2's mapping); the bitstream is copied from an
  MMAP CAPTURE buffer into the coded buffer. vaEndPicture queues;
  vaMapBuffer on the coded buffer and vaSyncSurface on the input wait.
  The stream starts with the first picture, when the input layout is
  known; every later input must have the same format and pitch.
- **Imported surfaces**: VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, one
  linear object, NV12 / P010 or one RGB plane (GNOME's screen recorder
  hands the compositor's XR24 buffers straight to the encoder).
- **CPU access to surfaces** goes through `DMA_BUF_IOCTL_SYNC` (detile,
  derived images on map / unmap, the video processor): heap buffers are
  cached and the codecs do not snoop. Without it, frames GStreamer
  uploaded into a derived image reached the encoder partly stale (the
  same CQP encode gave different bytes run to run).
- **Device discovery** scans `/dev/video*` for the cezanne decoder; the
  encoder is found by its sysfs name (`cezanne-venc`, no camera node is
  opened; `CEZANNE_VA_ENCODER` overrides). The
  media node is optional. Without it (Firefox's sandbox denies
  `/dev/media*`, and its seccomp filter blocks the media request ioctls),
  requests are allocated, queued and reinitialised through cezanne-vdec's
  private ioctls on the video node (`CEZANNE_VDEC_IOC_REQUEST_*`, in
  `cvd.h` and the module's `vdec.h`).
- **Capture.** Capture buffers are created lazily: CREATE_BUFS and STREAMON
  happen on the first picture, once the coded size is known.

## Testing

On the phone, from `mainline/vcodec/test/`:

- `va-cmp <clip>` decodes through ffmpeg
  (`-hwaccel vaapi -hwaccel_output_format vaapi`, then hwdownload) and
  compares against ffmpeg's software decode.
- `ff-va-test <url> [seconds]` runs Firefox with hardware decoding forced
  and with media, Dmabuf and sandbox logging. It prints a summary and takes
  a screenshot. `KEEP=1` leaves Firefox running.

- `va-enc-test [WxH] [format] [frames] [rate-control]` encodes raw frames
  with vah264enc and prints frames, colour tags, speed and PSNR
  (`VA_DRV=<build dir>` tests a build).
- `gnome-screencast.js FILE SECONDS` records with GNOME's recorder;
  `screencast-gst.js SECONDS PIPELINE` runs a pipeline on the same
  mutter screen-cast stream (gjs, as the session user).

`CEZANNE_VA_DEBUG=1` in the environment turns on the driver's debug log.
