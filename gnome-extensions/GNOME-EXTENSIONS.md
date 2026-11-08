# GNOME Shell extensions in the cezanne port

How the phone's shell is adapted: what the extensions are, where they live,
the conventions they follow, and how to test one without holding the phone.

## What exists

All phone-specific shell behaviour is done with extensions; GNOME Shell
itself is the Arch package (51.1, the same version as on the build host)
with one St patch, a CSS selector index (packaging/overlays/gnome-shell;
NOTES "GNOME startup stalls"). The extensions live in `mainline/extensions/<uuid>/`
and are packaged as cezanne-extensions (the `PKGBUILD` there; schemas
compiled at build time); the area column names the kernel / rootfs pieces
each one belongs to:

| uuid (`@arch-port`)        | area    | what it does |
|----------------------------|---------|--------------|
| cezanne-aod                | display | always-on-display clock in panel doze mode (lock screen too) |
| cezanne-autobrightness     | display | phone-style auto brightness from the ambient light sensor |
| cezanne-dc-dimming         | display | quick settings: DC dimming toggle, MIUI colour schemes |
| cezanne-fod                | display | under-display fingerprint spot, HBM on press; lock screen kept in portrait (lockPortrait.js) |
| cezanne-portrait-calendar  | display | clock popup stacked for portrait, stock layout in landscape |
| cezanne-app-grid           | display | phone app grid: 4x6 in portrait, 8x2/8x3 in landscape, largest icon that fits, full width, no workspace strip |
| cezanne-flashlight         | camera  | quick settings toggle for the rear flash LEDs |
| cezanne-notification-led   | leds    | notification / charging light and vibration via feedbackd (prefs + gschema) |
| cezanne-charging           | power   | charging watts next to the battery icon, PD / turbo marks |
| cezanne-game-awake         | power   | idle inhibitor while a Steam game runs (`reaper SteamLaunch`, from the launch on) or a window is fullscreen: no blank / lock / sleep mid-game; released when locked so the power key still sleeps |
| cezanne-osk                | input   | maximized windows shrink above the on-screen keyboard instead of sliding (stock slide leaves them drawn off their real position) |
| cezanne-nfc                | nfc     | quick settings NFC toggle; neard polls only while unlocked; tag / card notifications (UID, NDEF, Open / Copy), none while the NFC app has focus |
| cezanne-show-taps          | input   | Android "Show taps": a spot under every finger (Clutter event filter, never consumes events) |
| cezanne-overview-close     | input   | close button on every window in the overview all the time (stock: mouse hover only), for touch |
| cezanne-lock-keep          | display | smooth unlocking with third-party extensions: keeps lock-safe ones running on the lock screen, keeps dash-to-dock's stylesheet loaded while it is off there, enables it only after the lock screen has slid away |
| cezanne-warmup             | display | smooth first menus after a shell start: styles the quick settings and calendar menus in idle time ahead of their first open |
| cezanne-usb                | usb     | "Use USB for" dialog when a computer enumerates the port: video stream (UVC webcam of the screen or a camera), data storage (MTP), developer network, charge only; notification instead when locked / fullscreen; talks to cezanne-usbd (apps/cezanne-usb) |

Third-party extensions on the phone (dash-to-dock, auto-brightness-toggle)
come from extensions.gnome.org and are not part of the project, except
Caffeine (caffeine@patapon.info, packaged as
packaging/gnome-shell-extension-caffeine; screen-blank "never": the screen
stays on, and cezanne-aod refuses sleep while Caffeine's inhibitor exists,
NOTES "USB modes"). The user
turns extensions on and off with the Extensions app (`gnome-extensions-app`,
Arch extra). Order in `org.gnome.shell enabled-extensions`: dash-to-dock
last, cezanne-lock-keep before it (an extension the shell disables on lock
disables and re-enables every extension enabled after it). A third-party
extension that is safe on the lock screen (quick settings items only, no
app launches, nothing drawn over it) goes into cezanne-lock-keep's `KEEP`.

## Files and conventions

- `metadata.json`: `uuid` is `cezanne-<thing>@arch-port`, `name` starts
  with "Cezanne", `shell-version` is `["51"]`,
  `"session-modes": ["user", "unlock-dialog"]`. Without 'unlock-dialog'
  the shell disables the extension on every lock and enables it on unlock:
  its widgets are rebuilt and styled from scratch, and a stylesheet.css
  loaded / unloaded makes St restyle every actor of the shell. The unlock
  animation and the first menu after it stuttered from that (NOTES "GNOME:
  stutter after wake / unlock"). So every extension runs on the lock
  screen and is made safe there instead: no app launches, settings or
  hardware that must stay off while locked checked against
  `Main.sessionMode.isLocked` on `Main.sessionMode` 'updated' (cezanne-nfc
  turns the field off), nothing above the lock screen that leaks session
  content. Quick settings toggles appear in the lock screen's quick
  settings like the stock ones.
- `extension.js`: ES module, `export default class ... extends Extension`
  (from `resource:///org/gnome/shell/extensions/extension.js`) with
  `enable()` and `disable()`. Everything `enable()` changes is undone in
  `disable()`: signals disconnected, actors destroyed, wrapped methods and
  inline styles restored, timeouts removed. Enabling / disabling in the
  Extensions app or a gdm restart shows a leak fast.
- Optional: `prefs.js` + `schemas/` (run `glib-compile-schemas schemas/`
  and ship `gschemas.compiled`; see cezanne-notification-led), `icons/` for
  symbolic icons (cezanne-flashlight).
- The file starts with a comment saying what the extension does and why the
  phone needs it, naming the kernel / rootfs pieces it relies on (sysfs
  paths, services, D-Bus names) and the shell internals it touches.
  Constants at the top, private state as `this._name`, four-space indent,
  no debug output left in. Log state changes that matter with
  `console.log('<Name>: ...')`; they land in the gnome-shell journal.
- Startup: the shell enables extensions while its startup animation runs
  on the same main thread, so every enable is a stall in it. `enable()` /
  `disable()` are small wrappers: while `Main.layoutManager._startingUp`,
  wait for its 'startup-complete', then run `_enable()`; `disable()` drops
  a pending wait or runs `_disable()` (copy them from any extension).
  Exceptions: cezanne-app-grid (shapes the overview the animation shows),
  cezanne-lock-keep and cezanne-warmup (both only hook in). Third-party
  ones are not held back: dash-to-dock enabled after the animation
  re-zoomed the overview around its dock at its end. NOTES "GNOME startup stalls".
- Never block the shell: sysfs reads and writes go through async Gio calls
  (`Gio.File.load_contents_async`, `replace_contents_async`); an I2C device
  timing out has hung the shell before. Poll with
  `GLib.timeout_add_seconds` and remove the source in `disable()`.
- Changing shell behaviour: prefer adjusting the actors the shell already
  built (reparent, set properties, inline `style`) over rebuilding UI. When
  a shell method has to be wrapped (the calendar's `_rebuildCalendar`, for
  example), keep the original on `this`, call it from the wrapper, restore
  it in `disable()`. Private shell fields (`_calendar`, `_messageList`,
  `statusArea.quickSettings._system`, ...) are version-specific: re-check
  them against the sources after a GNOME upgrade.
- dash-to-dock replaces `ControlsManagerLayout.prototype._computeWorkspacesBoxForState`
  (and a few more) again on every `monitors-changed` (rotation), restoring
  the method it saved first: an override of ours made on that prototype
  after it is silently dropped. Wrap such methods on the instance
  (`controls.layout_manager`) and call the prototype's current method from
  the wrapper (cezanne-app-grid); `delete` the own property in `disable()`.
- Inline styles beat the theme: `actor.style = 'margin: 0; ...'`. An
  `St.BoxLayout`'s `spacing` comes from CSS (a programmatic
  `layout_manager.spacing` is reset on the next style change), so set it
  inline too. A theme `width:` acts as a minimum size (override `min-width`
  inline), and Clutter enlarges an allocation to an actor's minimum size
  instead of clipping. `x_fill` / `y_fill` are not Clutter properties
  (assigning them does nothing); use `x_expand` and `x_align`.
- A function in mutter's GIR is not necessarily exported by libmutter
  (`Meta.OrientationManager.inhibit_tracking` fails with "undefined
  symbol"): check with `nm -D --defined-only /usr/lib/libmutter-51.so.0`.
  `GObject.signal_handlers_block_matched(obj, {signalId})` in GJS takes
  the signal name, not the numeric id.
- `Main.layoutManager.addChrome()` / `trackChrome()` take only
  `trackFullscreen` and `affectsStruts` in GNOME 51; an unknown parameter
  (`affectsInputRegion`, from older shells) throws.
- Phone geometry: 1080x2400 at scale 2 = 540x1200 logical px, panel across
  the top, touch only, no keyboard. The right side of the panel has about
  190 px free for indicators. Portrait is the default; auto-rotation
  (iio-sensor-proxy, `orientation-lock` false) rotates the whole shell, so
  layout code listens to `Main.layoutManager` `monitors-changed` and reads
  `Main.layoutManager.primaryMonitor` (logical px) to tell portrait from
  landscape.
- Project rules apply: our own code only (GPL-2.0-or-later like the
  shell), nothing from MIUI, nothing committed or published.

## Reading the shell's own code

The shell's JavaScript is compiled into its library as a GResource; the
host has the same version as the phone. Extract what you need:

```sh
python3 - <<'PY'
import gi; gi.require_version('Gio', '2.0')
from gi.repository import Gio
data = open('/usr/lib/gnome-shell/libshell-51.so', 'rb').read()
open('/tmp/shell.gresource', 'wb').write(data[data.find(b'GVariant'):])
Gio.Resource.load('/tmp/shell.gresource')._register()
for n in ['ui/dateMenu.js', 'ui/calendar.js', 'ui/messageList.js',
          'ui/panel.js', 'ui/layout.js', 'ui/extensionSystem.js']:
    open('/tmp/' + n.split('/')[-1], 'wb').write(
        Gio.resources_lookup_data('/org/gnome/shell/' + n, 0).get_data())
PY
```

The theme is a separate file: load
`/usr/share/gnome-shell/gnome-shell-theme.gresource` the same way and read
`/org/gnome/shell/theme/gnome-shell-dark.css` (`-light.css` for the light
style). Grep it for the style classes you see in the JS to learn the
margins, paddings and widths you are working against. `gresource extract`
does the same when glib2-devel is installed.

## Installing on the phone

Extensions are per user, in
`/home/arch/.local/share/gnome-shell/extensions/` (no system-wide directory
is used). The session runs as user `arch` (uid 1002) with auto-login; its
bus is `unix:path=/run/user/1002/bus`. From the host:

```sh
cd ~/arch-port
R='runuser -u arch -- env XDG_RUNTIME_DIR=/run/user/1002 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1002/bus'
tar -C mainline/extensions -cf - cezanne-portrait-calendar@arch-port |
  scripts/arch-ssh.sh "D=/home/arch/.local/share/gnome-shell/extensions;
    tar -C \$D -xf - && chown -R arch:arch \$D/cezanne-portrait-calendar@arch-port"
scripts/arch-ssh.sh "$R gnome-extensions enable cezanne-portrait-calendar@arch-port"
```

- A new extension directory, and any change to `extension.js` of an
  installed one, is only picked up by a new shell: the shell imports each
  extension module once and `gnome-extensions enable` / `disable` do not
  reload it. Restart the session with `systemctl restart gdm` (logs the
  user out; the shell is back in about 5 s). Enabling an extension the
  shell has already loaded, and `prefs.js` / gsettings changes, work live.
  Kill leftover `gdm-session-worker [pam/gdm-password]` /
  `[pam/gdm-fingerprint]` processes (lock screen conversations) first, or
  the old session stays "closing" and GDM shows the greeter instead of the
  automatic login; if the greeter comes up anyway, restart gdm once more.
- `gnome-extensions enable` fails for a uuid the running shell has not
  loaded; writing `org.gnome.shell enabled-extensions` with gsettings as
  user arch works and takes effect at the next restart.
- `gnome-extensions disable` also writes the uuid into
  `disabled-extensions`, which wins over `enabled-extensions` (the
  extension stays INITIALIZED); `gnome-extensions enable` takes it out.
- Check the result with `$R gnome-extensions info <uuid>` (State: ACTIVE)
  and the journal:
  `journalctl -b -o cat /usr/bin/gnome-shell | grep -A5 "JS ERROR"`.

## Testing without holding the phone

Look at the screen first: the user may be using the phone (`scr`, below).

- `scr` (on the phone as `/usr/local/bin/scr`, copy in `devtools/`):
  kmsgrab screenshot of the scanout into `/tmp/scr.png`, scaled to 540 px
  wide, i.e. one image pixel per logical pixel. `scp` it to the host and
  view it, or measure positions with PIL. In landscape the image is the
  rotated framebuffer; rotate it on the host.
- `shell-input` (`devtools/shell-input` + `shell-input.js`, installed as
  `/usr/local/bin/shell-input` and `/usr/local/lib/cezanne/shell-input.js`):
  keyboard and pointer injection through mutter's
  `org.gnome.Mutter.RemoteDesktop` API (the phone has no uinput). Run it as
  root over ssh; it switches to the session user itself:

  ```sh
  shell-input key super+v sleep 900      # open the clock / notifications popup
  shell-input key Escape                 # close it
  shell-input moveto 480 16 click        # tap the quick settings indicator
  shell-input moveto 270 600 click       # tap at logical x,y
  shell-input scroll 3                   # wheel down 3 steps
  shell-input tap 270 600                # touchscreen tap at logical x,y
  shell-input press 270 600 800          # touch and hold 800 ms
  shell-input swipe 120 600 420 640 400  # one-finger touch drag over 400 ms
  shell-input size                       # logical screen size (540x1200 / 1200x540)
  ```
  While it runs the panel shows the orange screen-sharing indicator. Only
  relative pointer motion is available without a screen-cast stream, so
  `moveto` parks the pointer in the bottom-right corner first (the top-left
  one is the Activities hot corner) and moves from there; `click` parks it
  there again afterwards, because a pointer left resting on a panel
  indicator makes the shell switch a menu opened by key to that
  indicator's menu on the next tap inside it.
  Touch needs absolute positions, which mutter only takes relative to a
  screen-cast stream: the touch actions record the primary monitor in a
  screen-cast session tied to the remote desktop one (stream coordinates =
  logical px x scale). Never kill `shell-input` while a finger is down: a
  removed virtual touchscreen does not end its touches, the shell's
  gestures keep the dead touch (the overview stuck open, Escape ignored)
  and mutter aborts at the next shell shutdown (`clutter_sprite_finalize:
  assertion failed: (!priv->press_count)`). The script lifts the finger in
  its `finally`; only SIGKILL gets past that. Virtual devices also do not
  bring up the on-screen keyboard: when the removed device was the last
  one used, the shell's `_lastDeviceIsTouchscreen()` throws ("already
  disposed") and touch mode never turns on; OSK behaviour has to be tried
  with a finger.
- Test notifications, as the session user:

  ```sh
  $R gdbus call --session --dest org.freedesktop.Notifications \
    --object-path /org/freedesktop/Notifications \
    --method org.freedesktop.Notifications.Notify \
    "App" 0 "mail-unread-symbolic" "Title" "Body text" "[]" "{}" 3000
  ```
- Rotation without moving the phone (mutter DisplayConfig; transform 1 =
  landscape, 0 = portrait; the `1` after the serial = not persisted):

  ```sh
  DC="$R gdbus call --session --dest org.gnome.Mutter.DisplayConfig --object-path /org/gnome/Mutter/DisplayConfig --method org.gnome.Mutter.DisplayConfig"
  serial=$($DC.GetCurrentState | sed -n 's/^(uint32 \([0-9]*\),.*/\1/p')
  $DC.ApplyMonitorsConfig $serial 1 "[(0, 0, 2.0, 1, true, [('DSI-1', '1080x2400@121.718', {})])]" "{}"
  ```
  The serial changes with every configuration change: read it again before
  rotating back. The accelerometer takes over again the next time the phone
  is moved.
- Looking Glass and `org.gnome.Shell.Eval` are not used: Eval needs the
  shell's unsafe mode, which stays off on the phone. Inspect layout from
  screenshots and the shell sources; when a value really has to be seen,
  log it from the extension and read the journal.
- The edit-test loop is: copy the extension, `systemctl restart gdm`, wait
  for `gnome-extensions info` to report ACTIVE, drive the UI with
  `shell-input`, screenshot with `scr`.
- `shell-input` can't reach the lock screen (remote desktop sessions are
  inhibited there). There use uinput (needs uinput.ko from mainline/uinput):
  `powerkey [hold-ms [code]]` presses KEY_POWER (or another evdev key) and
  `touchswipe X1 Y1 X2 Y2 MS` swipes (panel px) through virtual devices
  (mainline/tools/*.c, /usr/local/bin). For timing tests keep one device:
  `touchswipe -d FIFO` / `powerkey -d FIFO` read commands from a FIFO; a new
  device per test makes the shell re-evaluate touch mode and rebuild the
  on-screen keyboard, and a touchscreen added while the display is off is
  mapped late (first touches lost).
- Frame timing: run the shell with MUTTER_DEBUG=kms (`systemctl --user
  set-environment MUTTER_DEBUG=kms` as arch, restart gdm; unset afterwards)
  and read the "[atomic] Committing update" lines' journal timestamps
  (`journalctl -o short-unix _PID=<shell>`): one per presented frame. Don't
  poll /sys/kernel/debug/dri/0/state for this, it takes the modeset locks.
- After a loop of gdm restarts check `gsettings get org.gnome.shell
  disable-user-extensions`: GNOME turns it on after startup "crashes" and
  every extension stays INITIALIZED.
- While the AOD is up the phone sleeps between minute updates and ssh only
  gets through on its wakes: hold `systemd-run --unit=hold
  systemd-inhibit --what=sleep sleep 1800` for interactive work (the AOD then
  stays awake in doze), `systemctl stop hold` to let it sleep again.

## Checklist for a new extension

1. Pick the uuid (`mainline/extensions/<uuid>/`), write `metadata.json` and
   `extension.js` as above; keep it to the one job the phone needs.
2. Read the relevant shell source and theme CSS first; note the private
   fields you depend on in the header comment.
3. Install, enable (gsettings) and restart gdm; check `gnome-extensions
   info` and the journal for JS errors.
4. Test with `shell-input` + `scr`; for lock-screen extensions also lock
   (`$R gdbus call --session --dest org.gnome.ScreenSaver --object-path
   /org/gnome/ScreenSaver --method org.gnome.ScreenSaver.Lock`) and suspend.
5. Disable and re-enable once (`gnome-extensions disable` / `enable`) to
   make sure `disable()` restores everything.
6. Write the result up in NOTES.md (what, why, gotchas) and add the
   extension to the table above.
