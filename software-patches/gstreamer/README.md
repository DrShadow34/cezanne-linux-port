# GStreamer v4l2codecs and va on the phone

gst-plugins-bad 1.28.8's stateless V4L2 decoders stamp the pooled buffers'
VideoMeta with the coded size (for example 1920x1088) under display-size
caps (1920x1080); videoconvert then rescales the coded picture to the caps
instead of cropping it. With the cezanne decoder every stream whose
display size is not a multiple of the 16x32 MM21 tile came out resampled
(1080p: 72 dB against a 1088-to-1080 bilinear scale of the correct
picture, 31 dB against the correct crop); the hardware output itself is
bit-exact (verified by untiling the raw capture buffers).

`0001-v4l2codecs-videometa-visible-size.patch` fixes the shared
output-state helper (two assignments).

`0002-v4l2codecs-dma-drm-tiled-pitch.patch`: with DMA_DRM caps (GTK's
gtkgstsink, so Snapshot's gallery, GtkVideo, Showtime) the buffers' VideoMeta
strides are taken as DRM plane pitches, but for the tiled MM21 format
GStreamer keeps them as packed tile counts (y_tiles << 16 | x_tiles):
Mesa's panfrost imported 1280x720 with a 1507408-byte pitch, detiled the
first tile row and read zeros below it, and every H.264 / AV1 video played
green. The patch converts tiled DMA_DRM meta strides to bytes.

`0003-va-h264enc-rgb-input.patch`: vah264enc took only 4:2:0 input. Our VA
driver's encoder (cezanne-venc underneath) reads RGB dma-bufs and converts
them to BT.709 itself, and lists VA_RT_FORMAT_RGB32 for H.264 EncSlice;
with RGB taken, GNOME's screen recorder (`pipewiresrc` DMA_DRM XR24 !
`vapostproc` ! `vah264enc`) runs vapostproc in passthrough and encodes the
compositor's buffers with no copy. The output caps get BT.709 colorimetry
(mp4mux tagged the stream GBR from the RGB input's).

`0004-va-encoder-free-pictures-of-imported-dmabufs.patch`: an upstream bug.
`gst_va_encode_picture_free()` took the VA display from the input buffer
and returned early without one; a dma-buf imported from another allocator
(pipewiresrc) has none, so every picture leaked its input, reconstructed
and coded buffers and the recording stopped after about 8 frames ("Failed
to create the reconstruct picture"). It falls back to the reconstructed
buffer's display.

`build.sh` rebuilds only the
v4l2codecs and va plugins from the upstream tarball with the patches and
installs them over the packaged `/usr/lib/gstreamer-1.0/libgstv4l2codecs.so`
and `libgstva.so` (the originals are kept as `.so.pkg`). Run it on the
phone as root:

```sh
curl -LO https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-1.28.8.tar.xz
sh build.sh gst-plugins-bad-1.28.8.tar.xz
```

Redo it after a gst-plugins-bad package update (or pin the package). Until
the patched plugin is installed, `mainline/vcodec/test/vdec-cmp` verifies
the decoder through GStreamer's copy path (which crops correctly) and its
own untiler, so cropped streams test bit-exact there while players that
use the zero-copy path still show the resampling.
