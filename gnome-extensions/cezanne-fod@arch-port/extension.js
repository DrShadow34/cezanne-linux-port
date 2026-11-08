// Fingerprint (FOD) spot for the cezanne panel.
//
// The optical sensor sits under the panel at the vendor's
// persist.vendor.sys.fp.fod.location.X_Y=439,1732 (202x202, panel pixels).
// States, set over D-Bus (org.cezanne.Fod.SetState on the session bus):
//   0 off, 1 icon (white fingerprint logo, e.g. while a reader waits),
//   2 press (white disc lighting the finger, panel in FOD HBM).
// The fingerprint service (cezanne-fpd, apps/cezanne-fp) publishes the
// state it wants as org.cezanne.Fod.State on the system bus
// (net.reactivated.Fprint /org/cezanne/Fod); the extension follows it, and
// switching the panel to FOD HBM after the dimmed frame is the "UI ready"
// the service waits for before it captures.
// On the lock screen GNOME starts authentication (password and
// fingerprint, gdm-fingerprint / pam_fprintd) only with the prompt, after a
// swipe; like a phone, the extension starts it behind the curtain whenever
// the lock screen is shown or the screen wakes (UnlockDialog's private
// _ensureAuthPrompt(): creates or restarts the prompt without showing it).
// With the AOD (cezanne-aod) the locked phone sleeps and the RTC wakes it
// every minute for the clock: armed 1.5 s after a resume only if it stayed
// awake and the panel left doze (the power key woke it).
// The password entry's focus opens the on-screen keyboard over the spot
// (after a swipe, before any tap on the entry): while the fingerprint
// service runs, Keyboard.open() on the lock screen is held back
// (InjectionManager) until the user taps the entry. Not tied to the spot's
// state: a swipe restarts GDM's conversations and the state drops to 0 for
// a moment, just when the entry takes the focus.
// For the press the panel HBM brightens everything, so the rest of the
// screen is covered by black at the alpha that brings it back to the
// current brightness; only the sensor disc gets the full HBM output.
// The lock screen stays in portrait, where the spot lines up with the
// sensor (lockPortrait.js).
import Cairo from 'cairo';
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as LoginManager from 'resource:///org/gnome/shell/misc/loginManager.js';
import {Keyboard} from 'resource:///org/gnome/shell/ui/keyboard.js';
import {UnlockDialog} from 'resource:///org/gnome/shell/ui/unlockDialog.js';
import {Extension, InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';

import {LockPortrait} from './lockPortrait.js';

const PANEL_W = 1080, PANEL_H = 2400;
const FOD_X = 439, FOD_Y = 1732, FOD_SIZE = 202;
const HBM = '/sys/bus/mipi-dsi/devices/1400e000.dsi.0/hbm';
const DOZE = '/sys/bus/mipi-dsi/devices/1400e000.dsi.0/doze';
const ARM_DELAY_MS = 1500;	// after a lock / wake / resume
const NOTIFICATIONS_GAP = 8;	// px between the spot and lock screen notifications
const BACKLIGHT = '/sys/class/backlight/1400e000.dsi.0';
const NORMAL_NITS = 500;	// backlight 2047, linear in DBV
const HBM_NITS = 650;		// FOD HBM (53 E0, 51 07FF), tuned by eye
const PRESS_TIMEOUT_MS = 3000;	// safety: never leave HBM on
const CORE = 1.15;		// solid white radius, x sensor radius
const GLOW = 1.8;		// fades out to this radius

const FodIface = `<node><interface name="org.cezanne.Fod">
  <method name="SetState"><arg type="u" direction="in" name="state"/></method>
  <property name="State" type="u" access="read"/>
</interface></node>`;

function readInt(path) {
    try {
        const [, b] = GLib.file_get_contents(path);
        return parseInt(new TextDecoder().decode(b));
    } catch {
        return -1;
    }
}

function writeStr(path, s) {
    try {
        const f = Gio.File.new_for_path(path);
        const out = f.replace(null, false, Gio.FileCreateFlags.NONE, null);
        out.write_all(new TextEncoder().encode(s), null);
        out.close(null);
    } catch (e) {
        console.log(`cezanne-fod: ${path}: ${e.message}`);
    }
}

export default class CezanneFod extends Extension {
    enable() {
        // The shell enables extensions while its startup animation runs on
        // the same main thread: build once it is over (NOTES "GNOME startup
        // stalls").
        if (Main.layoutManager._startingUp) {
            this._startupId = Main.layoutManager.connect('startup-complete', () => {
                Main.layoutManager.disconnect(this._startupId);
                this._startupId = 0;
                this._enable();
            });
            return;
        }
        this._enable();
    }

    disable() {
        if (this._startupId) {
            Main.layoutManager.disconnect(this._startupId);
            this._startupId = 0;
            return;
        }
        this._disable();
    }

    _enable() {
        this._state = 0;
        this._dbus = Gio.DBusExportedObject.wrapJSObject(FodIface, this);
        this._dbus.export(Gio.DBus.session, '/org/cezanne/Fod');
        this._nameId = Gio.DBus.session.own_name('org.cezanne.Fod',
            Gio.BusNameOwnerFlags.REPLACE, null, null);

        this._dim = new St.DrawingArea({reactive: false});
        this._dim.connect('repaint', a => this._paintDim(a));
        // reactive: a touch on the spot is for the sensor, not for what
        // lies under it (the on-screen keyboard below the password entry)
        this._icon = new St.Icon({
            icon_name: 'auth-fingerprint-symbolic',
            style: 'color: white;',
            reactive: true,
        });
        for (const a of [this._dim, this._icon]) {
            a.hide();
            global.stage.add_child(a);
        }
        Main.layoutManager.connectObject('monitors-changed',
            () => this._layout(), this);
        this._layout();

        this._portrait = new LockPortrait();
        this._login = LoginManager.getLoginManager();
        this._login.connectObject('prepare-for-sleep', (_lm, sleeping) => {
            if (!sleeping)
                this._armSoon();
        }, this);
        Main.screenShield.connectObject(
            'lock-screen-shown', () => {
                this._armSoon();
                this._watchDialog();
            },
            'wake-up-screen', () => this._armSoon(), this);
        this._watchDialog();
        global.backend.get_monitor_manager().connectObject(
            'power-save-mode-changed', () => this._armSoon(), this);
        this._armSoon();
        this._injections = new InjectionManager();
        const ext = this;
        this._injections.overrideMethod(Keyboard.prototype, 'open', open =>
            function (...args) {
                if (ext._holdKeyboard())
                    return undefined;
                return open.apply(this, args);
            });
        /*
         * Back on the clock page (swipe down, idle, the AOD) GNOME destroys
         * the auth prompt and with it the fingerprint conversation. The
         * spot works from the clock page: keep the prompt (rebuilding it
         * and its conversations cost ~1 s of shell time, a laggy swipe
         * right after a wake), only see that it is verifying again.
         */
        this._injections.overrideMethod(UnlockDialog.prototype,
            '_maybeDestroyAuthPrompt', destroy => function (...args) {
                if (!ext._service?.g_name_owner || !this._authPrompt ||
                    this !== Main.screenShield._dialog) {
                    destroy.apply(this, args);
                    return;
                }
                const focus = global.stage.key_focus;
                if (focus === null || this._authPrompt.contains(focus) ||
                    focus === this._otherUserButton)
                    this.grab_key_focus();
                GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
                    ext._armLockScreen();
                    return GLib.SOURCE_REMOVE;
                });
            });

        this._cancel = new Gio.Cancellable();
        Gio.DBusProxy.new_for_bus(Gio.BusType.SYSTEM,
            Gio.DBusProxyFlags.DO_NOT_AUTO_START, null,
            'net.reactivated.Fprint', '/org/cezanne/Fod', 'org.cezanne.Fod',
            this._cancel, (o, res) => {
                try {
                    this._service = Gio.DBusProxy.new_for_bus_finish(res);
                } catch (e) {
                    if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                        console.log(`cezanne-fod: fingerprint service: ${e.message}`);
                    return;
                }
                this._service.connectObject(
                    'g-properties-changed', () => this._follow(),
                    'notify::g-name-owner', () => this._follow(), this);
                this._follow();
            });
    }

    _armSoon() {
        if (this._armId)
            GLib.source_remove(this._armId);
        this._armId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, ARM_DELAY_MS, () => {
            this._armId = 0;
            this._armLockScreen();
            return GLib.SOURCE_REMOVE;
        });
    }

    // authentication behind the lock screen's curtain: locked, awake, out
    // of the AOD's doze
    _armLockScreen() {
        const dialog = Main.screenShield._dialog;
        if (!Main.screenShield.locked || !dialog?._ensureAuthPrompt ||
            this._login.preparingForSleep || readInt(DOZE) > 0)
            return;
        if (!dialog._authPrompt)
            console.log('cezanne-fod: lock screen: starting authentication');
        dialog._ensureAuthPrompt();
        this._watchEntry();
    }

    // the lock screen's password entry: a press on it asks for the keyboard
    _watchEntry() {
        const entry = Main.screenShield._dialog?._authPrompt?._entry;
        if (!entry || entry === this._entry)
            return;
        this._entry = entry;
        this._keyboardWanted = false;
        entry.connectObject('captured-event', (_a, event) => {
            const type = event.type();
            if (type === Clutter.EventType.BUTTON_PRESS ||
                type === Clutter.EventType.TOUCH_BEGIN) {
                this._keyboardWanted = true;
                Main.keyboard.open(Main.layoutManager.focusIndex);
            }
            return Clutter.EVENT_PROPAGATE;
        }, 'destroy', () => {
            this._entry = null;
        }, this);
    }

    // no keyboard over the spot on the lock screen, unless asked for
    _holdKeyboard() {
        if (!Main.screenShield.locked || !this._service?.g_name_owner)
            return false;
        this._watchEntry();
        return !this._keyboardWanted;
    }

    // the service's state; nothing when it is not running
    _follow() {
        const v = this._service.g_name_owner
            ? this._service.get_cached_property('State') : null;
        this.SetState(v ? v.unpack() : 0);
    }

    _disable() {
        this._portrait.destroy();
        this._portrait = null;
        if (this._armId)
            GLib.source_remove(this._armId);
        this._unwatchDialog();
        Main.screenShield.disconnectObject(this);
        global.backend.get_monitor_manager().disconnectObject(this);
        this._login.disconnectObject(this);
        this._injections.clear();
        this._entry?.disconnectObject(this);
        this._entry = null;
        this._cancel.cancel();
        this._service?.disconnectObject(this);
        this._service = null;
        this.SetState(0);
        Main.layoutManager.disconnectObject(this);
        this._dim.destroy();
        this._icon.destroy();
        this._dim = this._icon = null;
        Gio.DBus.session.unown_name(this._nameId);
        this._dbus.unexport();
        this._dbus = null;
    }

    /*
     * The lock screen's notifications grow up from the bottom edge, where
     * the auth indicator ("Unlock with fingerprint", bottom start) also
     * sits on the narrow portrait screen, and over the fingerprint spot:
     * keep them above the spot (the clock moves up as the list grows).
     */
    _watchDialog() {
        const dialog = Main.screenShield._dialog;
        const indicator = dialog?._authIndicatorButton;
        if (!indicator || !dialog._notificationsBox || indicator === this._indicator)
            return;
        this._unwatchDialog();
        this._indicator = indicator;
        this._notifications = dialog._notificationsBox;
        indicator.connectObject(
            'destroy', () => this._unwatchDialog(false), this);
        this._placeNotifications();
    }

    // restore: false when the dialog goes away with them
    _unwatchDialog(restore = true) {
        this._indicator?.disconnectObject(this);
        if (restore && this._notifications)
            this._notifications.style = null;
        this._indicator = this._notifications = null;
    }

    // stage px -> CSS px: St scales CSS lengths by the scale factor
    _placeNotifications() {
        const m = this._monitor;
        if (!m || !this._rect)
            return;
        const scale = St.ThemeContext.get_for_stage(global.stage).scale_factor;
        const pad = m.y + m.height - this._rect.y + NOTIFICATIONS_GAP * scale;
        this._notifications.style = `padding-bottom: ${Math.ceil(pad / scale)}px;`;
    }

    get State() {
        return this._state;
    }

    // panel pixel rect of the sensor in stage coordinates (portrait only:
    // the lock screen and the reader prompts don't rotate)
    _layout() {
        const m = Main.layoutManager.primaryMonitor;
        if (!m)
            return;
        const s = m.width / PANEL_W;
        this._rect = {
            x: m.x + FOD_X * s, y: m.y + FOD_Y * s, size: FOD_SIZE * s,
        };
        this._dim.set_position(m.x, m.y);
        this._dim.set_size(m.width, m.height);
        this._monitor = m;
        const iconSize = Math.round(this._rect.size * 0.7);
        this._icon.icon_size = iconSize;
        this._icon.set_position(
            Math.round(this._rect.x + (this._rect.size - iconSize) / 2),
            Math.round(this._rect.y + (this._rect.size - iconSize) / 2));
    }

    // black alpha that maps HBM output back to the current brightness;
    // the blend runs on gamma-encoded values, luminance ~ value^2.2
    _dimAlpha() {
        const bl = readInt(`${BACKLIGHT}/brightness`);
        const max = readInt(`${BACKLIGHT}/max_brightness`);
        if (bl < 0 || max <= 0)
            return 0;
        const nits = NORMAL_NITS * bl / max;
        return 1 - Math.min(1, nits / HBM_NITS) ** (1 / 2.2);
    }

    _paintDim(area) {
        const cr = area.get_context();
        const [w, h] = area.get_surface_size();
        const m = this._monitor;
        const r = this._rect;
        const cx = r.x - m.x + r.size / 2, cy = r.y - m.y + r.size / 2;

        cr.setOperator(Cairo.Operator.SOURCE);
        cr.setSourceRGBA(0, 0, 0, this._alpha);
        cr.rectangle(0, 0, w, h);
        cr.fill();
        // solid over the sensor, then fading into the dim layer
        const rad = r.size / 2;
        const g = new Cairo.RadialGradient(cx, cy, rad * CORE,
            cx, cy, rad * GLOW);
        g.addColorStopRGBA(0, 1, 1, 1, 1);
        g.addColorStopRGBA(0.35, 1, 1, 1, 0.55);
        g.addColorStopRGBA(1, 0, 0, 0, this._alpha);
        cr.setSource(g);
        cr.arc(cx, cy, rad * GLOW, 0, 2 * Math.PI);
        cr.fill();
        cr.setSourceRGBA(1, 1, 1, 1);
        cr.arc(cx, cy, rad * CORE, 0, 2 * Math.PI);
        cr.fill();
        cr.$dispose();
    }

    _raise() {
        for (const a of [this._dim, this._icon])
            global.stage.set_child_above_sibling(a, null);
    }

    SetState(state) {
        if (state === this._state)
            return;
        const prev = this._state;
        this._state = state;
        if (this._pressTimeout) {
            GLib.source_remove(this._pressTimeout);
            this._pressTimeout = 0;
        }

        if (prev === 2)
            this._hbmOff();

        if (state === 2) {
            this._alpha = this._dimAlpha();
            this._dim.queue_repaint();
            this._dim.show();
            this._icon.hide();
            this._raise();
            // switch the panel once the dimmed frame is on its way
            const laterId = global.stage.connect('after-paint', () => {
                global.stage.disconnect(laterId);
                if (this._state === 2)
                    this._hbmOn();
            });
            this._pressTimeout = GLib.timeout_add(GLib.PRIORITY_DEFAULT,
                PRESS_TIMEOUT_MS, () => {
                    this._pressTimeout = 0;
                    this.SetState(1);
                    return GLib.SOURCE_REMOVE;
                });
        } else {
            this._dim.hide();
            this._icon.visible = state === 1;
            this._raise();
            if (state === 1 && this._holdKeyboard())
                Main.keyboard.close();
        }
        this._dbus?.emit_property_changed('State',
            new GLib.Variant('u', state));
    }

    _hbmOn() {
        this._savedHbm = readInt(HBM);
        writeStr(HBM, '2');
    }

    _hbmOff() {
        // back to the sunlight boost (1) if it was on
        writeStr(HBM, this._savedHbm === 1 ? '1' : '0');
    }
}
