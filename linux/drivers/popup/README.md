# Pop-up front camera mechanism (Redmi K30 Ultra)

Hardware: TI DRV8846 stepper driver (step pulses on PWM channel 2 = GPIO152,
dir GPIO185, nSLEEP GPIO186, mode GPIO188/184, power GPIO128) and an AKM
AK09970 3-axis hall sensor on i2c9 0x0d (reset GPIO27) reading the camera
carriage magnet.

## Kernel

- `popup-motor.ko` (popup_motor.c): platform driver "xiaomi,cezanne-popup-motor".
  sysfs `move` ("up" / "down" = MIUI profile: 554/559 ms at a 54171 ns step
  period + 50 ms slow phase; "up <ms>" / "down <ms>" nudges; "stop"),
  `state` (idle/up/down, pollable). Timing runs in an hrtimer, every move is
  capped at 800 ms, so userspace cannot leave the motor running.
- `ak09970.ko` (ak09970.c): IIO "popup_hall", in_magn_{x,y,z}_raw in counts
  (1.1 uT), continuous 100 Hz while read, off after 3 s idle.
- Needs pwm-mediatek (MT6885 PWM = MT8183 layout) and patch 0019
  (i2c-mt65xx multi-channel controllers, see NOTES.md).
- Build: `./build.sh`.

## Userspace

- `cezanne-popupd(.service)`: raises the camera while it is in use, retracts
  otherwise. Clients send `hold` on /run/cezanne-popup.sock and keep the
  connection; when the process exits or crashes the socket closes and the
  camera retracts after 0.5 s. CAMERA_DEVICES (front sensor nodes, once a
  camera driver exists) also count as "in use" while any process has one
  open.
- Safety: retract at start/stop (also after a daemon crash: systemd restarts
  it), every move verified against MIUI's per-unit hall calibration
  (/var/lib/cezanne-popup/hall_calidata.txt from persist:/sensors/), one
  retry then a notification; pushed in by hand (position < 7.9 for 2 samples
  while up; it rests at 8.00-8.02, so a light push is enough) and
  free fall (|a| < 3 m/s^2 for 60 ms) retract it.
- Rate limit: 5 pop-ups in 60 s -> notification about frequent use, 10 in
  120 s -> 60 s cool-down (protects the motor), like MIUI's warning.
- `cezanne-popup status|up|down|hold [cmd]` for testing and scripts.
