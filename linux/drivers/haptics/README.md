# Vibrator (Awinic AW8697 LRA)

- Chip: AW8697 (ID 0x97) at 0x5a on vendor i2c7 (our i2c-6, 11d03000), reset
  GPIO129 (high = running), IRQ GPIO22 (unused). The vendor DT also has an
  AW8624 on i2c6 for other boards.
- `aw8697.c`: native out-of-tree driver (register map / sequences from the GPL
  vendor driver). Build: `scripts/remote-build.sh module mainline/haptics`,
  install to `/lib/modules/$(uname -r)/extra/` + `depmod -a`; autoloads from DT
  (`awinic,aw8697`).
- Firmware: `aw8697_haptic.bin` (vendor waveform library, 11 waves) in
  `/usr/lib/firmware` (rootfs copy in mainline/rootfs/usr/lib/firmware).
- Input device `aw8697-haptics` (FF):
  - FF_RUMBLE / FF_CONSTANT / FF_PERIODIC sine: buzz for replay.length
    (RAM loop of wave 11, battery-compensated)
  - FF_PERIODIC + FF_CUSTOM, custom_data[0] = 0..9: short library effects
  - FF_GAIN
- sysfs (`/sys/bus/i2c/devices/6-005a/`): `f0` (0.1 Hz), `trim` (LRA osc
  trim, -1 = factory efuse), `calibrate` (write 1: measure F0, set trim;
  ~110 ms buzz, F0-detect method). Calibrated: F0 164.0 Hz, trim 49, saved in /var/lib/cezanne-haptics/trim and applied at bind (udev 65-cezanne-haptics.rules, /usr/local/bin/cezanne-haptics-trim).
- `hapctl.c`: test tool, `hapctl rumble <ms> [%]`, `hapctl effect <0..9> [%]`
  (on the phone in /usr/local/bin).
- feedbackd (extra/feedbackd) drives the vibrator and the notification light; the GNOME extension cezanne-notification-led routes notifications/charging to it. rootfs etc/udev/rules.d/72-feedbackd.rules
  overrides the packaged rules (vibra + rgb:status; the packaged LED rules would grab
  both AW2015 pop-up LEDs and point at /usr/libexec/fbd-ledctrl, Arch has /usr/lib).
