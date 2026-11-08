# Licence of software-patches/

The patches are changes to other projects and carry those projects'
licences. The packaging around them (`PKGBUILD`, `cezanne.sh`, `upstream`,
helper scripts, the build system in `packaging/`) is **MIT**
([../LICENSES/MIT.txt](../LICENSES/MIT.txt)), the repository's licence.

| Directory | Upstream project | Licence of the patches / sources |
|---|---|---|
| `mesa/` | Mesa (Panfrost, PanVK) | MIT |
| `fex-guestos-mesa/` | Mesa, built for FEX's x86 guest | MIT |
| `libcamera/` | libcamera | LGPL-2.1-or-later |
| `mutter/` | mutter | GPL-2.0-or-later |
| `gnome-shell/` | GNOME Shell | GPL-2.0-or-later |
| `plymouth/` | Plymouth | GPL-2.0-or-later |
| `modemmanager/` | ModemManager | GPL-2.0-or-later |
| `xorg-xwayland/` | Xwayland | MIT (X11) |
| `mission-center/` | Mission Center | GPL-3.0-or-later |
| `mangohud/` | MangoHud | MIT |
| `gstreamer/` | GStreamer (gst-plugins-bad) | LGPL-2.1-or-later |
| `libva-v4l2-request/` | fork of Bootlin's libva-v4l2-request (the VA-API driver cezanne-va) | LGPL-2.1-or-later and MIT, per file (`COPYING`, `COPYING.LGPL`, `COPYING.MIT` there) |
| `neard/` | neard (packaging, a D-Bus policy change at build time) | GPL-2.0-only |
| `realesrgan-ncnn-vulkan/` | Real-ESRGAN ncnn Vulkan | MIT |
| `umtp-responder/` | uMTP-Responder (packaging only) | GPL-3.0-or-later |
| `gnome-shell-extension-caffeine/` | Caffeine (packaging only) | GPL-2.0-or-later |
| `steam-arm64/` | Valve's Steam client (packaging only) | proprietary (Steam Subscriber Agreement), downloaded from Valve at build time and not redistributed here |
| `packaging/` | this port's build system | MIT |

Licence texts: [../LICENSES/](../LICENSES/).
