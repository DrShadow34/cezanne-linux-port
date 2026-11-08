# Licence of linux/

Everything here is kernel work and is licensed **GPL-2.0-only**
([../LICENSES/GPL-2.0-only.txt](../LICENSES/GPL-2.0-only.txt)) unless a
file's own `SPDX-License-Identifier` or the table below says otherwise.

| Path | Licence |
|---|---|
| `patches/` | GPL-2.0-only, as the Linux files they change. The new dt-bindings headers `include/dt-bindings/reset/mt6885-resets.h` (0002) and `include/dt-bindings/power/mt6885-power.h` (0009): GPL-2.0-only OR BSD-2-Clause |
| `dts/` | GPL-2.0-only |
| `drivers/` | GPL-2.0-only; `drivers/uinput/uinput.c` GPL-2.0-or-later (from Linux) |
| `drivers/connectivity/` | GPL-2.0-only: patches to MediaTek's GPL connectivity drivers (Wi-Fi, Bluetooth, GNSS, FM) and the vendor sources they apply to |
| `drivers/fingerprint/teei/` | GPL-2.0-only: Microtrust TEE driver from Xiaomi's GPL kernel release, ported |
| `boot/` | GPL-2.0-only: boot shim, initramfs init, boot image and rescue system scripts |
| `packaging/` | GPL-2.0-only: the linux-cezanne PKGBUILD, its hook and helper |
| `system/cezanne-base/` | GPL-2.0-or-later ([../LICENSES/GPL-2.0-or-later.txt](../LICENSES/GPL-2.0-or-later.txt)), as its PKGBUILD declares; the libcamera tuning files in `etc/libcamera/ipa/simple/` are CC0-1.0 |

No firmware is included. The firmware the drivers load comes from the
phone's stock partitions; see the Firmware column of `../hardware.md`.
