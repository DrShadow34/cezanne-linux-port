# Shell

The phone runs [niri](https://github.com/niri-wm/niri) with the port's own
shell, cezanne-shell (`shell/cezanne-shell`). The shell is one
[Quickshell](https://quickshell.org) (Qt Quick) process. It draws the panel,
quick menu, app grid, dock, notifications, lock screen and always-on display.
The other parts:
- squeekboard is the on-screen keyboard for apps;
- swayidle handles idle;
- greetd runs the login screen in a separate niri instance;
- X11 apps run through xwayland-satellite.

The shell's README lists every part and its file.

## Migrated from GNOME

Until 2026-11-13 the phone ran GNOME: mutter and gnome-shell with patches,
plus the port's GNOME Shell extensions. The switch to niri saved memory and
gave a UI made for the phone. Because of that history, some links to GNOME
are still there:

- **Directory names.** `gnome-extensions/` and `gnome-apps/` kept their
  names.
  - The extensions belong to GNOME Shell and don't run under niri. The shell
    has its own version of each (table below).
  - `gnome-apps/` holds the port's own apps: the FM radio, the gallery,
    storage, power and so on.
- **The apps still rely on GTK4 / libadwaita**, and some on other GNOME
  parts:
  - the camera is GNOME Snapshot;
  - the gallery uses LocalSearch;
  - settings live in GSettings / dconf;
  - "Show in Files" opens Nautilus.

  They run under niri unchanged.
- **GNOME settings and interfaces the shell reuses**:
  - GNOME's dark style (`color-scheme`), which GTK apps follow;
  - GNOME's automatic brightness setting (`ambient-enabled`);
  - the screen-keyboard setting;
  - the dconf keys of the old notification LED and NFC extensions.

  The quick menu's Settings button opens GNOME Settings
  (`gnome-control-center`, started with `XDG_CURRENT_DESKTOP=GNOME`).
  Screen recording uses `org.gnome.Mutter.ScreenCast`, which niri implements.
- **Patches made for the GNOME session**: `software-patches/mutter` and
  `software-patches/gnome-shell` (hardware pre-rotation, no lag on the first
  menus). The niri session doesn't use them. They remain built while the
  GNOME packages are installed, because `cezanne-base` still depends on
  gnome-shell.
- **Docs**: HARDWARE.md, the README files and the commit history may still
  name a GNOME extension or setting as the UI for a feature.

## GNOME extensions and their shell counterparts

| Extension (`gnome-extensions/`) | In cezanne-shell |
|---|---|
| cezanne-aod | `Aod.qml`, `bin/cezanne-aod-ctl` (same mechanism: fade to black, doze, clock, sleep through `/sys/power/state`) |
| cezanne-fod | `LockScreen.qml`: fingerprint unlock (pam_fprintd on cezanne-fpd), the sensor spot, HBM on press; the lock screen stays portrait (`Rotation.qml`) |
| cezanne-autobrightness | `System.qml` (the quick menu's "A" next to brightness) |
| cezanne-dc-dimming | quick menu: DC Dimming, Colors |
| cezanne-flashlight | quick menu: Flashlight |
| cezanne-nfc | `Nfc.qml`, quick menu: NFC |
| cezanne-notification-led | `NotificationLight.qml` (feedbackd) |
| cezanne-charging | the quick menu's battery pill: charge type as bolts, watts |
| cezanne-usb | `Usb.qml`, `UsbDialog.qml` ("Use USB for") |
| cezanne-game-awake | `shell.qml`: a fullscreen window holds an idle inhibitor |
| cezanne-show-taps | `Touch.qml`, `cezanne-touch.c` (also three-finger gestures and edge back) |
| cezanne-app-grid | `Launcher.qml` |
| cezanne-portrait-calendar | `CalendarView.qml` |
| cezanne-osk | squeekboard; the lock screen has its own keyboard (`Keyboard.qml`) |
| cezanne-overview-close | the dock's long press: Close |
| cezanne-lock-keep, cezanne-warmup | not needed: they worked around GNOME Shell |

Other GNOME features the shell provides itself:
- the screenshot / screencast UI (`Screenshot.qml`, `cezanne-screencast.c`);
- the polkit agent;
- the volume OSD;
- the calendar with notifications;
- Caffeine;
- the geoclue agent.

## Not there (yet)

- No fingerprint enrolment: GNOME didn't have one for this sensor either.
- No night light: niri has none.
- No screencast of an area: niri can't record one. The recorder records the
  monitor, and the VA driver cuts the selection out.

## Memory

The shell's allocators are tuned (`systemd/user/cezanne-shell.service`,
`cezanne-osk.service`, `niri.service.d`):
- **qs (jemalloc):** one arena, `MALLOC_CONF=narenas:1,...`.
- **squeekboard and niri (glibc):** `MALLOC_ARENA_MAX=2`. niri's
  `config.kdl` removes the variable again for the apps it starts.

The calls / SMS stack starts only with a SIM inserted (`gnome-apps/cezanne-modem`, `cezanne-sim-watch`).
With these, the whole session went from 1221 to 824 MB PSS.

## Testing without the screen

`test/sandbox.sh` in cezanne-shell runs niri nested in a headless cage at
1080x2400. It takes screenshots with grim and drives the shell through its
IPC (`cezanne-shell-ctl`), using the `*.steps` files.

`CEZANNE_*` variables replace everything that touches hardware or the user's
files with dummies, for example:
- the AOD;
- the fingerprint;
- the backlight;
- USB;
- the lock file.

The nested niri can't screencast (no GBM), so recordings are tested on the
phone.
