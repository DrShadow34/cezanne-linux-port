# Pop-up camera RGB lights (AW2015)

- `leds-aw2015.c`: out-of-tree driver, build with
  `scripts/remote-build.sh module mainline/leds`, install to
  `/lib/modules/$(uname -r)/kernel/drivers/leds/` + `depmod -a`.
- DT: `led-controller@64` on i2c9 (left, `rgb:status`, notification light,
  `awinic,charge-indicator`) and i2c7 (right, `rgb:indicator`).
- sysfs: `multi_intensity` "r g b", `brightness`; timer trigger = hardware
  blink; pattern trigger `hw_pattern`:
  - `0 rise B hold B fall 0 off` breathing (times 40..8300 ms, quantised)
  - `B t B t B t B t` steady, `0 0 0 0 0 0 0 0` off
- Needs `ledtrig-pattern` (CONFIG_LEDS_TRIGGER_PATTERN=m,
  rootfs etc/modules-load.d/cezanne-leds.conf) and udev
  64-cezanne-rgb.rules for the video group.
- GNOME: rootfs/leds/cezanne-notification-led@arch-port (schemas compiled
  with glib-compile-schemas).
- Details, findings and TODO (notifications in suspend): NOTES.md
  "Notification light".
