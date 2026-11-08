// SPDX-License-Identifier: GPL-2.0-or-later
// Routes desktop notifications and the charging state to feedbackd, which
// drives the pop-up camera's notification light (left edge, rgb:status,
// AW2015) and the vibrator (AW8697):
// - a new notification vibrates (feedbackd event notification-new-generic,
//   with the app's ID so feedbackd's per-app profiles apply);
// - the light fades in and out while there are unread notifications (ones
//   that arrived since the last unlock or since the notification list was
//   last opened, and are not dismissed), in the colour of the newest one's
//   app (preferences);
// - otherwise, on the charger: steady red while charging, green when charged.
// Do Not Disturb suppresses both. Stock GNOME Shell sends feedbackd nothing,
// hence this extension.
//
// feedbackd's LED feedback has a fixed colour per event, so the extension
// generates a theme ($XDG_CONFIG_HOME/feedbackd/themes/cezanne.json, parent
// "default") with one event per colour in use, x-cezanne-led-rrggbb, plus
// x-cezanne-charging / x-cezanne-charged, and selects it in feedbackd's
// settings. A light state is one such event triggered with timeout 0
// (loops until ended). Without a session (greeter, powered off) the chip
// shows its own red charging light. feedbackd fades in software (kernel
// pattern trigger), so the fade stops while suspended.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Lfb from 'gi://Lfb';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as MessageTray from 'resource:///org/gnome/shell/ui/messageTray.js';

const LED = '/sys/class/leds/rgb:status';
const THEME = 'cezanne';
const CHARGING_EVENT = 'x-cezanne-charging';
const CHARGED_EVENT = 'x-cezanne-charged';
const NEW_EVENT = 'notification-new-generic';
// fade period: 1000 / FADE_MHZ s up, as long down
const FADE_MHZ = 300;

// UPower device states
const CHARGING = 1;
const FULLY_CHARGED = 4;
const PENDING_CHARGE = 5;

const UPowerDeviceIface = `<node>
  <interface name="org.freedesktop.UPower.Device">
    <property name="State" type="u" access="read"/>
    <property name="Percentage" type="d" access="read"/>
    <property name="IsPresent" type="b" access="read"/>
  </interface>
</node>`;
const UPowerDeviceProxy = Gio.DBusProxy.makeProxyWrapper(UPowerDeviceIface);

Gio._promisify(Gio.File.prototype, 'replace_contents_async');
Gio._promisify(Gio.File.prototype, 'load_contents_async');
Gio._promisify(Lfb.Event.prototype, 'trigger_feedback_async');
Gio._promisify(Lfb.Event.prototype, 'end_feedback_async');

function sleep(ms) {
    return new Promise(resolve => GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
        resolve();
        return GLib.SOURCE_REMOVE;
    }));
}

// Ending a feedback writes brightness 0, which makes the kernel drop the
// pattern trigger; udev (72-feedbackd.rules, fbd-ledctrl) sets it again
// shortly after. A blink started before that cannot write its pattern and
// stays steady, so wait for the trigger.
async function patternTrigger() {
    const file = Gio.File.new_for_path(`${LED}/trigger`);
    await sleep(100); // feedbackd may write after replying
    for (let tries = 0; tries < 40; tries++) {
        try {
            const [bytes] = await file.load_contents_async(null);
            if (new TextDecoder().decode(bytes).includes('[pattern]'))
                return;
        } catch {}
        await sleep(50);
    }
}

function normalize(color) {
    const m = /^#([0-9a-f]{6})$/i.exec(color ?? '');
    return m ? `#${m[1].toLowerCase()}` : null;
}

function colorEvent(color) {
    return `x-cezanne-led-${color.slice(1)}`;
}

class NotificationLight {
    constructor(settings) {
        this._settings = settings;
        this._lit = new Map(); // notification -> colour, oldest first
        this._wanted = null;   // LED event name
        this._event = null;    // the running Lfb.Event
        this._queue = Promise.resolve();
        this._notifSettings = new Gio.Settings({schema_id: 'org.gnome.desktop.notifications'});
        this._fbdSettings = new Gio.Settings({schema_id: 'org.sigxcpu.feedbackd'});

        Lfb.init('org.gnome.Shell');
        this._updateTheme();

        Main.messageTray.connectObject('source-added',
            (tray, source) => this._watchSource(source), this);
        for (const source of Main.messageTray.getSources())
            this._watchSource(source);

        Main.screenShield?.connectObject('locked-changed', () => {
            if (!this._locked())
                this._clear();
        }, this);
        // the notification list
        Main.panel.statusArea.dateMenu?.menu.connectObject('open-state-changed',
            (menu, open) => {
                if (open)
                    this._clear();
            }, this);
        this._notifSettings.connectObject('changed::show-banners', () => this._sync(), this);
        this._settings.connectObject('changed', (s, key) => {
            if (key === 'notification-color' || key === 'app-colors')
                this._updateTheme();
            this._sync();
        }, this);

        // feedbackd restarted: its light state is gone
        this._fbdWatch = Gio.bus_watch_name(Gio.BusType.SESSION, 'org.sigxcpu.Feedback',
            Gio.BusNameWatcherFlags.NONE, null, () => {
                this._event = null;
                this._wanted = null;
                this._sync();
            });

        this._battery = new UPowerDeviceProxy(Gio.DBus.system, 'org.freedesktop.UPower',
            '/org/freedesktop/UPower/devices/DisplayDevice', (proxy, error) => {
                if (error)
                    console.warn(`Notification light: ${error.message}`);
                this._sync();
            });
        this._battery.connectObject('g-properties-changed', () => this._sync(), this);

        this._sync();
    }

    destroy() {
        Main.messageTray.disconnectObject(this);
        for (const source of Main.messageTray.getSources())
            source.disconnectObject(this);
        Main.screenShield?.disconnectObject(this);
        Main.panel.statusArea.dateMenu?.menu.disconnectObject(this);
        this._notifSettings.disconnectObject(this);
        this._settings.disconnectObject(this);
        this._battery.disconnectObject(this);
        Gio.bus_unwatch_name(this._fbdWatch);
        if (this._batteryRetry)
            GLib.source_remove(this._batteryRetry);
        this._batteryRetry = 0;
        this._lit.clear();
        this._set(null);
        this._queue.finally(() => Lfb.uninit());
    }

    _colors() {
        const colors = new Set([normalize(this._settings.get_string('notification-color'))]);
        for (const c of Object.values(this._settings.get_value('app-colors').deepUnpack()))
            colors.add(normalize(c));
        colors.delete(null);
        return [...colors].sort();
    }

    // (re)write the feedbackd theme; selecting it makes feedbackd reload
    _updateTheme() {
        const led = (event, color, frequency) =>
            ({'event-name': event, type: 'Led', color, frequency});
        const theme = {
            name: THEME,
            'parent-name': 'default',
            profiles: [{
                name: 'silent',
                feedbacks: [
                    led(CHARGING_EVENT, '#ff0000', 0),
                    led(CHARGED_EVENT, '#00ff00', 0),
                    ...this._colors().map(c => led(colorEvent(c), c, FADE_MHZ)),
                ],
            }],
        };
        const contents = `${JSON.stringify(theme, null, 2)}\n`;
        const dir = GLib.build_filenamev([GLib.get_user_config_dir(), 'feedbackd', 'themes']);
        const file = Gio.File.new_for_path(GLib.build_filenamev([dir, `${THEME}.json`]));
        this._queue = this._queue.then(async () => {
            let old = null;
            try {
                old = new TextDecoder().decode((await file.load_contents_async(null))[0]);
            } catch {}
            if (old !== contents) {
                GLib.mkdir_with_parents(dir, 0o755);
                await file.replace_contents_async(new TextEncoder().encode(contents),
                    null, false, Gio.FileCreateFlags.NONE, null);
            }
            if (old !== contents || this._fbdSettings.get_string('theme') !== THEME) {
                // the running light event belongs to the old theme
                if (this._event) {
                    const event = this._event;
                    this._event = null;
                    await event.end_feedback_async(null).catch(() => {});
                    await patternTrigger();
                }
                this._wanted = null;
                // a theme change is what makes feedbackd reload
                this._fbdSettings.set_string('theme', 'default');
                this._fbdSettings.set_string('theme', THEME);
            }
        }).catch(e => console.warn(`Notification light: ${e.message}`))
            .then(() => this._sync());
    }

    _locked() {
        return Main.screenShield?.locked ?? false;
    }

    _clear() {
        this._lit.clear();
        this._sync();
    }

    _watchSource(source) {
        source.connectObject(
            'notification-added', (s, notification) => this._onAdded(source, notification),
            'notification-removed', (s, notification) => {
                this._lit.delete(notification);
                this._sync();
            }, this);
    }

    _appColor(source) {
        // app sources (GTK / portal and FDO notifications from a known app)
        const appId = source.policy?.id;
        const colors = this._settings.get_value('app-colors').deepUnpack();
        return normalize(appId && colors[appId]) ||
            normalize(this._settings.get_string('notification-color'));
    }

    _doNotDisturb() {
        return !this._notifSettings.get_boolean('show-banners');
    }

    _onAdded(source, notification) {
        if (notification.urgency === MessageTray.Urgency.LOW || notification.isTransient)
            return;
        this._lit.delete(notification);
        this._lit.set(notification, this._appColor(source));
        if (!this._doNotDisturb() && this._settings.get_boolean('vibrate'))
            this._vibrate(source.policy?.id);
        this._sync();
    }

    _vibrate(appId) {
        const event = new Lfb.Event({event: NEW_EVENT});
        if (appId)
            event.set_app_id(appId);
        event.trigger_feedback_async(null)
            .catch(e => console.warn(`Notification light: ${e.message}`));
    }

    _batteryEvent() {
        if (!this._settings.get_boolean('battery-light'))
            return null;
        // at login UPower's properties may come later, without a change signal
        if (this._battery.State === null && !this._batteryRetry) {
            let tries = 0;
            this._batteryRetry = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 1, () => {
                if (this._battery.State === null && ++tries < 30)
                    return GLib.SOURCE_CONTINUE;
                this._batteryRetry = 0;
                this._sync();
                return GLib.SOURCE_REMOVE;
            });
        }
        if (!this._battery.IsPresent)
            return null;
        const state = this._battery.State;
        if (state !== CHARGING && state !== FULLY_CHARGED && state !== PENDING_CHARGE)
            return null;
        if (state === FULLY_CHARGED ||
            this._battery.Percentage >= this._settings.get_int('charged-percentage'))
            return CHARGED_EVENT;
        return CHARGING_EVENT;
    }

    _sync() {
        const newest = [...this._lit.values()].pop();
        if (newest && !this._doNotDisturb())
            this._set(colorEvent(newest));
        else
            this._set(this._batteryEvent());
    }

    // one light event at a time: end the old one, then start the new one
    _set(name) {
        if (name === this._wanted)
            return;
        this._wanted = name;
        console.log(`Notification light: ${name ?? 'off'}`);
        this._queue = this._queue.then(async () => {
            if (this._event) {
                const old = this._event;
                this._event = null;
                await old.end_feedback_async(null);
            }
            if (name && name === this._wanted) {
                await patternTrigger();
                const event = new Lfb.Event({event: name, timeout: 0});
                await event.trigger_feedback_async(null);
                this._event = event;
            }
        }).catch(e => console.warn(`Notification light: ${e.message}`));
    }
}

export default class NotificationLightExtension extends Extension {
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
        this._light = new NotificationLight(this.getSettings());
    }

    _disable() {
        this._light?.destroy();
        this._light = null;
    }
}
