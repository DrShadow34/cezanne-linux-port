# cezanne-usb: "Use USB for" on the Redmi K30 Ultra

When a computer (not a charger) is plugged into the USB-C port, the phone
asks what the connection is for:

| Mode | What the computer sees |
|------|------------------------|
| Video stream | a USB webcam ("Redmi K30 Ultra"): the phone's screen, or one of its cameras (main, ultrawide, macro, front) |
| Data storage | an MTP device with the home folder (`/home/arch`), files owned by the user |
| Developer network | an RNDIS network, the phone at 172.16.42.1 (ssh) |
| Charge only | nothing (Escape or dismissing the dialog does the same) |

The dialog only comes up unlocked and with nothing fullscreen. Otherwise,
or when nothing is chosen within 120 s, the port charges only and a
notification offers the other modes. While connected, a quiet resident
notification shows the mode; tapping it opens the dialog again.

## Webcam (OBS and other UVC clients)

Formats offered, per source (format 1 is the default):

| Source | MJPEG | YUYV (uncompressed) |
|--------|-------|---------------------|
| cameras (landscape, cropped to 16:9) | 1920x1080, 1280x720 @ 30 / 15 fps | 640x360 @ 15 fps |
| screen (portrait) | 1080x2400, 720x1600 @ 30 / 15 fps | 360x800 @ 10 fps |

- For HD, pick **Motion-JPEG** (OBS: Video Format "Motion-JPEG"). A client
  that does not ask for a format gets the small YUYV one.
- The link carries 8 MB/s (one 1024-byte packet per USB microframe: the
  phone's USB controller cannot do the high-bandwidth mode webcams usually
  use). JPEG quality adapts so each frame fits a frame interval, so the
  frame rate holds and the quality drops for noisy, dark scenes.
- Measured 2026-11-04 from a Linux laptop: cameras 1080p and 720p MJPEG
  30 fps, YUYV 15 fps; screen ~2 fps when nothing moves (GNOME only sends
  frames on changes), 10-13 fps with heavy animation (the compositor's
  screencast rate, not the link or the encoder).
- The screen stream keeps the phone from locking itself; when it is locked
  anyway (power key) the frames stop until it is unlocked. Stopping the
  screen share from the top-bar indicator ends the mode (charge only).
- The camera (the front one pops up) only runs while the computer reads
  the webcam, plus 5 s (OBS reopens the device when its settings change).

## Data storage

uMTP-Responder (packaging/umtp-responder) serves the home folder. On Linux
it shows up in the file manager (gvfs-mtp); `gio list
mtp://Arch_Linux_ARM_Redmi_K30_Ultra_<serial>/Home/` from a shell.

## Known limits

- A computer that has power-managed the idle phone (Linux laptops with USB
  autosuspend, Windows selective suspend) may not notice a mode change
  made later on the phone: replug the cable. (The phone's disconnect does
  not reach a suspended bus; NOTES "USB modes".)
- High-speed USB only (the device tree sets the port's maximum speed to
  high speed).

## Pieces

- `cezanne-usbd` (root, `org.cezanne.Usb` on the system bus, polkit
  `org.cezanne.usb.set-mode` for the active session): owns the configfs
  gadget, tells a computer from a charger (a host configuring the probe
  gadget), switches modes, runs umtprd / dnsmasq, holds a sleep inhibitor
  while a mode serves a host.
- `cezanne-usb-stream` (user unit, started by the extension in stream
  mode): the UVC device side (controls, probe / commit, stream on / off on
  the main loop) and the GStreamer pipeline (pipewiresrc from mutter's
  ScreenCast, or libcamerasrc) into the gadget's buffers.
- The GNOME Shell extension `cezanne-usb@arch-port`
  (mainline/rootfs/usb, in cezanne-base): dialog and notifications.
- Kernel: linux-cezanne >= 7.3.1-4 (patches 0067-0070: mtu3 ep0, GPD
  ring, f_uvc alt 0, interrupt storm after a suspend without a gadget).

Test from a computer: `v4l2-ctl -d /dev/videoN --list-formats-ext`,
`v4l2-ctl --set-fmt-video=width=1920,height=1080,pixelformat=MJPG
--stream-mmap --stream-count=60 --verbose` (per-frame sizes and rate);
switch modes from ssh with `systemd-run --user -M arch@ --wait --pipe
busctl call org.cezanne.Usb /org/cezanne/Usb org.cezanne.Usb SetMode ss
stream main`.
