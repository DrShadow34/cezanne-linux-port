// Always-on display for the cezanne panel (MIUI's AOD, the dirty way).
//
// The panel has a doze mode (sysfs doze): it shows its frame memory at a
// fixed low brightness by itself and stays in it through crtc disable and
// suspend. So, on the way into suspend (logind PrepareForSleep, held with a
// delay inhibitor): draw a black screen with the clock, let that frame go
// out, switch the panel to doze, arm the RTC for the next minute and let
// the system sleep. Woken at the alarm: move the clock (burn-in), redraw,
// sleep again. Woken by the power key: leave doze, remove the overlay, the
// lock screen is there. Woken by anything else (Wi-Fi, Bluetooth): let it
// do its work for a moment and sleep again, the panel stays in doze.
//
// Once in the AOD the extension sleeps by itself (writes to
// /sys/power/state), like Android's autosleep, not through logind: on every
// logind resume NetworkManager takes Wi-Fi down and re-associates (WoWLAN
// devices are taken down after the wake), and geoclue's BeaconDB lookup
// after each reconnect woke the phone again a few seconds later. So the
// power key is the extension's too while it is enabled (gsd's suspend
// would also act on the press that woke the phone): unlocked or on the
// lock screen it locks and goes to the AOD, in the AOD it wakes up.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';
import Clutter from 'gi://Clutter';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as QuickSettings from 'resource:///org/gnome/shell/ui/quickSettings.js';
import * as LoginManager from 'resource:///org/gnome/shell/misc/loginManager.js';
import {GnomeShell} from 'resource:///org/gnome/shell/ui/shellDBus.js';
import {UnlockDialog} from 'resource:///org/gnome/shell/ui/unlockDialog.js';
import {Extension, InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';

const DOZE = '/sys/bus/mipi-dsi/devices/1400e000.dsi.0/doze';
const WAKEALARM = '/sys/class/rtc/rtc0/wakealarm';
/*
 * Sleep without logind (the video group may: tmpfiles cezanne-aod.conf).
 * No wakeup_count handshake: the Wi-Fi driver's wakelocks count as
 * wakeup events, ~30/s and always some from its own suspend notifier,
 * so every suspend was aborted. As with logind, only a wake interrupt
 * aborts it (write fails, EBUSY). Deep (SPM, ~85 mA vs ~150 mA s2idle)
 * on battery only: with VBUS present mtu3 can't let its IP sleep and
 * refuses it (s2idle keeps it powered instead). A deep attempt refused
 * without a wake interrupt: s2idle for the next minute.
 */
const USE_DEEP = true;
const DEEP_BACKOFF_S = 60;
const SLEEP_DEEP_SH = 'echo deep > /sys/power/mem_sleep && echo mem > /sys/power/state';
const SLEEP_S2IDLE_SH = 'echo freeze > /sys/power/state';
const WAKEUP_IRQ = '/sys/power/pm_wakeup_irq';
const PMIC_IRQ = 'mt6358-irq';	// its action: power key, charger, RTC alarm
const POWER_KEYCODE = 124;	// KEY_POWER + 8
const WOKEN_MS = 1500;		// the press that woke it comes in after resume
const REST_MS = 1000;		// woken by Wi-Fi & co: awake this long
const RETRY_MS = 1000;		// suspend aborted by a wakeup event: again
const INHIBITED_MS = 5000;	// sleep inhibited: awake in the AOD, look again
const SESSION_MANAGER = 'org.gnome.SessionManager';
const SESSION_MANAGER_PATH = '/org/gnome/SessionManager';
const INHIBIT_SUSPEND = 4;
const INHIBIT_IDLE = 8;
const CAFFEINE_APP_ID = 'caffeine-gnome-extension';
const LUX = '/sys/bus/iio/devices/iio:device5/in_illuminance_input';
const BATTERY = '/sys/class/power_supply/battery';
const STATE = GLib.build_filenamev([GLib.get_user_config_dir(), 'cezanne', 'aod']);
/*
 * Settings of the cezanne-power app (schema installed by it): AOD on / off,
 * clock brightness, Wi-Fi in standby. Without it: the old state file.
 */
const POWER_SCHEMA = 'org.cezanne.power';
const NM = 'org.freedesktop.NetworkManager';
const NM_PATH = '/org/freedesktop/NetworkManager';
const BLUEZ = 'org.bluez';
const BT_ADAPTER = '/org/bluez/hci0';
const BRIGHT_LUX = 50;		// doze bright above this ambient light
/*
 * Auto doze level: light read when the AOD comes up and then at most this
 * often. scp_iio turns the sensor off in every suspend (left on, the hub
 * woke the 26 MHz clock for each sample: ~16 mA), so a read waits ~0.4 s
 * for the first sample.
 */
const LUX_EVERY_S = 300;
// burn-in shift: the clock box moves anywhere inside the screen minus 5% at
// the sides and 20% at the top and the bottom
const AREA_X = 0.05;
const AREA_Y = 0.20;
const ALARM_SLACK = 3;		// s: woken this close to the alarm = RTC wake
/*
 * Locked and idle: suspend (into the AOD) before gsd-power blanks the
 * screen (SCREENSAVER_TIMEOUT_BLANK, 30 s): from that blank only input
 * wakes it and the power key suspends, so the phone sat dark and awake.
 */
const LOCKED_IDLE_MS = 20000;
const FADE_BLACK_MS = 350;	// entering: the screen fades to black,
const FADE_CLOCK_MS = 400;	// then the clock fades in

function readStr(path, fallback = '') {
    try {
        const [, b] = GLib.file_get_contents(path);
        return new TextDecoder().decode(b).trim();
    } catch {
        return fallback;
    }
}

// '' / 'fast' (USB PD contract above 5 V into the MT6360) / 'direct' (the
// BQ25970 charge pump switching: PPS direct charging, rootfs cezanne-dcharge)
function fastCharging() {
    const ps = '/sys/class/power_supply';
    if (readStr(`${ps}/bq25970/status`) === 'Charging')
        return 'direct';
    let dir, name, fast = '';
    try {
        dir = GLib.Dir.open(ps, 0);
    } catch {
        return '';
    }
    while ((name = dir.read_name()) !== null) {
        // online 1 = fixed PDO, 2 = PPS
        if (name.startsWith('tcpm-source-psy-') &&
            readStr(`${ps}/${name}/online`) !== '0' &&
            parseInt(readStr(`${ps}/${name}/voltage_now`)) > 5500000)
            fast = 'fast';
    }
    dir.close();
    // QC 2.0 / 3 charger raised above 5 V (cezanne-dcharge)
    if (parseInt(readStr(`${ps}/xmusb350/qc_voltage`)) > 5500)
        fast = 'fast';
    return fast;
}

/*
 * The doze attribute runs a panel command sequence (~50 ms, a 40 ms wait in
 * it): written from a GIO worker thread, not the shell's main loop, which
 * would drop frames right when the lock screen comes up.
 */
function writeStrAsync(path, s) {
    return new Promise(resolve => {
        Gio.File.new_for_path(path).replace_contents_bytes_async(
            new GLib.Bytes(new TextEncoder().encode(s)), null, false,
            Gio.FileCreateFlags.NONE, null, (file, res) => {
                try {
                    file.replace_contents_finish(res);
                    resolve(true);
                } catch (e) {
                    console.log(`cezanne-aod: ${path}: ${e.message}`);
                    resolve(false);
                }
            });
    });
}

// a D-Bus property, or null
function getProperty(bus, name, path, iface, prop) {
    return new Promise(resolve => {
        bus.call(name, path, 'org.freedesktop.DBus.Properties', 'Get',
            new GLib.Variant('(ss)', [iface, prop]), null,
            Gio.DBusCallFlags.NONE, -1, null, (conn, res) => {
                try {
                    resolve(conn.call_finish(res).deepUnpack()[0].deepUnpack());
                } catch (e) {
                    console.log(`cezanne-aod: ${prop}: ${e.message}`);
                    resolve(null);
                }
            });
    });
}

function callMethod(bus, name, path, iface, method, args = null) {
    return new Promise(resolve => {
        bus.call(name, path, iface, method, args, null, Gio.DBusCallFlags.NONE, -1, null,
            (conn, res) => {
                try {
                    resolve(conn.call_finish(res).deepUnpack());
                } catch (e) {
                    console.log(`cezanne-aod: ${method}: ${e.message}`);
                    resolve(null);
                }
            });
    });
}

/*
 * Caffeine (packaging/gnome-shell-extension-caffeine) holds an idle
 * inhibitor by default (screen kept on), which says nothing about sleep;
 * the user turns it on to keep the phone awake, so the power key must not
 * put it to sleep either.
 */
async function caffeineActive() {
    const [paths] = await callMethod(Gio.DBus.session, SESSION_MANAGER, SESSION_MANAGER_PATH,
        SESSION_MANAGER, 'GetInhibitors') ?? [[]];
    const ids = await Promise.all(paths.map(path => callMethod(Gio.DBus.session,
        SESSION_MANAGER, path, `${SESSION_MANAGER}.Inhibitor`, 'GetAppId')));
    return ids.some(id => id?.[0] === CAFFEINE_APP_ID);
}

function setProperty(bus, name, path, iface, prop, value) {
    return new Promise(resolve => {
        bus.call(name, path, 'org.freedesktop.DBus.Properties', 'Set',
            new GLib.Variant('(ssv)', [iface, prop, value]), null,
            Gio.DBusCallFlags.NONE, -1, null, (conn, res) => {
                try {
                    conn.call_finish(res);
                    resolve(true);
                } catch (e) {
                    console.log(`cezanne-aod: set ${prop}: ${e.message}`);
                    resolve(false);
                }
            });
    });
}

// a Bluetooth device connected (headset, controller); unknown counts as yes
function btConnected() {
    return new Promise(resolve => {
        Gio.DBus.system.call(BLUEZ, '/', 'org.freedesktop.DBus.ObjectManager',
            'GetManagedObjects', null, null, Gio.DBusCallFlags.NONE, -1, null,
            (conn, res) => {
                try {
                    const [objects] = conn.call_finish(res).recursiveUnpack();
                    resolve(Object.values(objects).some(o =>
                        o['org.bluez.Device1']?.Connected));
                } catch (e) {
                    console.log(`cezanne-aod: bluez: ${e.message}`);
                    resolve(true);
                }
            });
    });
}

function powerSettings() {
    const schema = Gio.SettingsSchemaSource.get_default()?.lookup(POWER_SCHEMA, true);
    return schema ? new Gio.Settings({settings_schema: schema}) : null;
}

// a USB cable in (the charger sees VBUS)
function vbusPresent() {
    const ps = '/sys/class/power_supply';
    let dir, name, vbus = false;
    try {
        dir = GLib.Dir.open(ps, 0);
    } catch {
        return false;
    }
    while ((name = dir.read_name()) !== null) {
        if (name.startsWith('mt6360-chg') && readStr(`${ps}/${name}/online`) === '1')
            vbus = true;
    }
    dir.close();
    return vbus;
}

function writeStr(path, s) {
    try {
        GLib.mkdir_with_parents(GLib.path_get_dirname(path), 0o755);
        const out = Gio.File.new_for_path(path).replace(null, false,
            Gio.FileCreateFlags.NONE, null);
        out.write_all(new TextEncoder().encode(s), null);
        out.close(null);
        return true;
    } catch (e) {
        console.log(`cezanne-aod: ${path}: ${e.message}`);
        return false;
    }
}

const AodToggle = GObject.registerClass(
class AodToggle extends QuickSettings.QuickToggle {
    constructor(settings) {
        super({
            title: 'Always-on Display',
            iconName: 'preferences-system-time-symbolic',
            toggleMode: true,
        });
        if (!settings) {
            this.checked = readStr(STATE, '1') === '1';
            this.connect('notify::checked',
                () => writeStr(STATE, this.checked ? '1\n' : '0\n'));
            return;
        }
        // the state file of older versions, once
        const old = readStr(STATE);
        if (old) {
            settings.set_boolean('aod-enabled', old === '1');
            GLib.unlink(STATE);
        }
        settings.bind('aod-enabled', this, 'checked', Gio.SettingsBindFlags.DEFAULT);
    }
});

const AodIndicator = GObject.registerClass(
class AodIndicator extends QuickSettings.SystemIndicator {
    constructor(settings) {
        super();
        this.toggle = new AodToggle(settings);
        this.quickSettingsItems.push(this.toggle);
    }

    destroy() {
        this.quickSettingsItems.forEach(i => i.destroy());
        super.destroy();
    }
});

export default class CezanneAod extends Extension {
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
        this._settings = powerSettings();
        this._indicator = new AodIndicator(this._settings);
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);

        this._overlay = new St.Widget({
            style: 'background-color: black;',
            reactive: true,		// swallow input while shown
            layout_manager: new Clutter.FixedLayout(),
        });
        this._box = new St.BoxLayout({orientation: Clutter.Orientation.VERTICAL});
        this._time = new St.Label({
            style: 'color: white; font-size: 64pt; font-weight: 200;',
            x_align: Clutter.ActorAlign.CENTER,
        });
        this._date = new St.Label({
            style: 'color: #b0b0b0; font-size: 15pt;',
            x_align: Clutter.ActorAlign.CENTER,
        });
        this._battery = new St.Label({
            style: 'color: #808080; font-size: 12pt; padding-top: 8px;',
            x_align: Clutter.ActorAlign.CENTER,
        });
        this._box.add_child(this._time);
        this._box.add_child(this._date);
        this._box.add_child(this._battery);
        this._overlay.add_child(this._box);
        this._overlay.hide();
        global.stage.add_child(this._overlay);
        this._spot = [0.5, 0.5];
        Main.layoutManager.connectObject('monitors-changed', () => {
            if (this._overlay.visible)
                this._place();
        }, this);

        this._active = false;	// overlay up, panel in doze
        this._entering = false;	// _sleep() on its way into the AOD
        this._alarm = 0;	// epoch s of the armed RTC alarm
        this._minute = -1;	// minute the clock shows
        this._sleeper = null;	// the SLEEP_*_SH process
        this._deep = false;	// it sleeps deep
        this._deepRefused = 0;	// epoch s of the last refused deep attempt
        this._checking = false;	// _sleepAllowed() in flight
        this._restId = 0;
        this._wokeAt = 0;	// monotonic us of the last wake to the lock screen
        this._idleCookie = null;	// gnome-session idle inhibitor (0: pending)
        this._aodSince = 0;	// epoch s the AOD came up
        this._wifiOff = false;	// Wi-Fi turned off here for standby
        this._btOff = false;	// Bluetooth turned off here for standby
        this._luxLevel = '1';	// auto doze level from the last light read
        this._luxAt = 0;	// epoch s of that read
        // a shell that went away in the AOD left the panel dozing
        if (readStr(DOZE, '0') !== '0')
            writeStr(DOZE, '0');
        writeStr(WAKEALARM, '0');
        this._login = LoginManager.getLoginManager();
        this._login.connectObject('prepare-for-sleep',
            (_lm, aboutToSuspend) => this._onSleep(aboutToSuspend), this);
        this._takeInhibitor();

        // the power key: gsd-media-keys' grab, relayed by the shell
        const aod = this;
        this._injections = new InjectionManager();
        this._injections.overrideMethod(GnomeShell.prototype,
            '_emitAcceleratorActivated', relay => function (action, ...args) {
                if (action === global.display.get_keybinding_action(POWER_KEYCODE, 0) &&
                    aod._onPowerKey())
                    return;
                relay.call(this, action, ...args);
            });
        // the lock screen's clock page opens the password prompt on any key,
        // the power key's own press included (it arrives after the wake)
        this._injections.overrideMethod(UnlockDialog.prototype,
            '_onKeyPress', onKeyPress => function (...args) {
                const [, keyval] = this._keyController.get_key();
                if (keyval === Clutter.KEY_PowerOff)
                    return Clutter.EVENT_PROPAGATE;
                return onKeyPress.apply(this, args);
            });

        this._idleMonitor = global.backend.get_core_idle_monitor();
        Main.screenShield.connectObject('locked-changed', () => {
            this._syncLockedIdle();
            if (!Main.screenShield.locked && this._active)
                this._wake();	// unlocked under the AOD (loginctl)
        }, this);
        this._syncLockedIdle();
        // blanked anyway while locked: to sleep (the AOD unblanks)
        global.backend.get_monitor_manager().connectObject(
            'power-save-mode-changed', () => {
                if (Main.screenShield.locked && !this._active) {
                    this._powerSave().then(mode => {
                        if (mode !== 0)
                            this._sleep();
                    });
                }
            }, this);
    }

    // mutter's DPMS state over D-Bus (0 on)
    _powerSave(mode) {
        const iface = 'org.gnome.Mutter.DisplayConfig';
        const args = mode === undefined
            ? ['Get', new GLib.Variant('(ss)', [iface, 'PowerSaveMode'])]
            : ['Set', new GLib.Variant('(ssv)', [iface, 'PowerSaveMode',
                new GLib.Variant('i', mode)])];
        return new Promise(resolve => {
            Gio.DBus.session.call(iface, '/org/gnome/Mutter/DisplayConfig',
                'org.freedesktop.DBus.Properties', args[0], args[1], null,
                Gio.DBusCallFlags.NONE, -1, null, (conn, res) => {
                    try {
                        const v = conn.call_finish(res);
                        resolve(mode === undefined
                            ? v.deepUnpack()[0].deepUnpack() : mode);
                    } catch (e) {
                        console.log(`cezanne-aod: PowerSaveMode: ${e.message}`);
                        resolve(-1);
                    }
                });
        });
    }

    _syncLockedIdle() {
        if (this._idleWatch) {
            this._idleMonitor.remove_watch(this._idleWatch);
            this._idleWatch = 0;
        }
        if (Main.screenShield.locked) {
            this._idleWatch = this._idleMonitor.add_idle_watch(
                LOCKED_IDLE_MS, () => this._sleep());
        }
    }

    _disable() {
        if (this._idleWatch)
            this._idleMonitor.remove_watch(this._idleWatch);
        this._idleWatch = 0;
        this._injections.clear();
        this._injections = null;
        this._stopSleeping();
        Main.screenShield.disconnectObject(this);
        global.backend.get_monitor_manager().disconnectObject(this);
        Main.layoutManager.disconnectObject(this);
        this._login.disconnectObject(this);
        this._login = null;
        this._exit();
        this._btResume();
        this._releaseInhibitor();
        this._overlay.destroy();
        this._overlay = null;
        if (this._settings)
            Gio.Settings.unbind(this._indicator.toggle, 'checked');
        this._settings = null;
        this._indicator.destroy();
        this._indicator = null;
    }

    async _takeInhibitor() {
        if (this._inhibitor)
            return;
        try {
            this._inhibitor = await this._login.inhibit('Always-on display', null);
        } catch (e) {
            console.log(`cezanne-aod: inhibit: ${e.message}`);
        }
    }

    _releaseInhibitor() {
        this._inhibitor?.close(null);
        this._inhibitor = null;
    }

    _enabled() {
        return this._indicator.toggle.checked;
    }

    // logind suspend (power menu, or the power key with the AOD off)
    _onSleep(aboutToSuspend) {
        if (aboutToSuspend) {
            this._stopSleeping();
            if (!this._enabled()) {
                this._btStandby().then(() => this._releaseInhibitor());
                return;
            }
            (this._active ? this._update() : this._open())
                .then(() => this._btStandby())
                .then(() => this._releaseInhibitor());
            return;
        }

        // resumed
        this._wokeAt = GLib.get_monotonic_time();
        this._takeInhibitor().then(() => {
            if (this._active && this._enabled()) {
                this._onWake(true);
            } else {
                this._exit();
                this._btResume();
            }
        });
    }

    // locked: into the AOD and to sleep (not enabled: plain suspend)
    async _sleep() {
        if (this._active || this._entering || this._login.preparingForSleep)
            return;
        if (!this._enabled()) {
            this._suspend();
            return;
        }
        this._entering = true;
        await this._open();
        this._entering = false;
        if (this._active)
            this._kernelSleep();
        else if (this._overlay?.visible)
            this._suspend();	// no doze: just sleep, any wake shows the lock screen
    }

    async _open() {
        // the overlay needs the display on (blanked lock screen)
        const shown = await this._powerSave() > 0;
        if (shown) {
            // overlay first: the first frame on is black
            this._show(false);
            await this._powerSave(0);
            await new Promise(r => GLib.timeout_add(
                GLib.PRIORITY_DEFAULT, 300, () => r() ?? false));
        }
        await this._enter(shown);
    }

    /*
     * What logind would refuse (a sleep block inhibitor) or gsd-power not
     * do automatically (an application's suspend inhibitor, e.g. music
     * playing), or Caffeine on: stay awake in the AOD instead.
     */
    async _sleepAllowed() {
        const [block, actions, caffeine] = await Promise.all([
            getProperty(Gio.DBus.system, 'org.freedesktop.login1',
                '/org/freedesktop/login1', 'org.freedesktop.login1.Manager',
                'BlockInhibited'),
            getProperty(Gio.DBus.session, SESSION_MANAGER, SESSION_MANAGER_PATH,
                SESSION_MANAGER, 'InhibitedActions'),
            caffeineActive(),
        ]);
        return !(block ?? '').split(':').includes('sleep') &&
            !((actions ?? 0) & INHIBIT_SUSPEND) && !caffeine;
    }

    async _kernelSleep() {
        if (this._sleeper || this._restId || this._checking)
            return;
        this._checking = true;
        const allowed = await this._sleepAllowed();
        if (allowed) {
            await this._wifiStandby();
            await this._btStandby();
        }
        this._checking = false;
        if (!this._active || this._sleeper || this._restId)
            return;		// woken, or busy otherwise meanwhile
        if (!allowed) {
            this._rest(INHIBITED_MS);
            return;
        }
        this._deep = USE_DEEP && !vbusPresent() &&
            Date.now() / 1000 - this._deepRefused > DEEP_BACKOFF_S;
        let proc;
        try {
            proc = Gio.Subprocess.new(['/bin/sh', '-c',
                this._deep ? SLEEP_DEEP_SH : SLEEP_S2IDLE_SH],
                Gio.SubprocessFlags.STDERR_SILENCE);
        } catch (e) {
            console.log(`cezanne-aod: sleep: ${e.message}`);
            this._suspend();
            return;
        }
        this._sleeper = proc;
        proc.wait_async(null, (p, res) => {
            try {
                p.wait_finish(res);
            } catch {}
            if (this._sleeper !== p)
                return;		// stopped
            this._sleeper = null;
            this._onWake(p.get_if_exited() && p.get_exit_status() === 0);
        });
    }

    _stopSleeping() {
        if (this._restId)
            GLib.source_remove(this._restId);
        this._restId = 0;
        // not yet written mem: no suspend after all (else it's past)
        this._sleeper?.force_exit();
        this._sleeper = null;
    }

    /*
     * slept: false = the suspend was aborted: by a wake interrupt on the
     * way in (a power key press too), or a device that refused
     */
    _onWake(slept) {
        if (!this._active)
            return;
        const irq = readStr(WAKEUP_IRQ, '0');	// cleared as each attempt starts
        if (!slept && irq === '0') {
            if (this._deep)
                this._deepRefused = Date.now() / 1000;
            this._rest(RETRY_MS);
            return;
        }
        const pmic = readStr(`/sys/kernel/irq/${irq}/actions`) === PMIC_IRQ;
        const rtc = this._alarm &&
            Math.abs(Date.now() / 1000 - this._alarm) <= ALARM_SLACK;
        if (pmic && !rtc)
            this._wake();
        else
            this._rest(rtc ? 0 : REST_MS);
    }

    // awake for ms, then the clock (minute passed) and sleep again
    _rest(ms) {
        if (this._restId)
            GLib.source_remove(this._restId);
        this._restId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, ms, () => {
            this._restId = 0;
            this._update().then(() => {
                if (this._active && !this._restId)
                    this._kernelSleep();
            });
            return GLib.SOURCE_REMOVE;
        });
    }

    // out of the AOD to the lock screen, as a logind resume would
    _wake() {
        this._wokeAt = GLib.get_monotonic_time();
        this._stopSleeping();
        this._exit();
        Main.screenShield._wakeUpScreen?.();
    }

    // true: handled here, not relayed to gsd-media-keys
    _onPowerKey() {
        if (!this._enabled())
            return false;
        if (GLib.get_monotonic_time() - this._wokeAt < WOKEN_MS * 1000)
            return true;	// the press that woke it
        if (this._active) {
            this._wake();
        } else {
            // animated, as on a logind suspend (no fade: the screen just cut)
            if (!Main.screenShield.locked)
                Main.screenShield.lock(true);
            this._sleep();
        }
        return true;
    }

    // t: unix time to show
    _layout(t) {
        this._minute = Math.floor(t / 60);
        const now = GLib.DateTime.new_from_unix_local(t);
        this._time.text = now.format('%H:%M');
        this._date.text = now.format('%a, %-d %B');
        const cap = readStr(`${BATTERY}/capacity`);
        const status = readStr(`${BATTERY}/status`);
        let charging = '';
        if (status === 'Charging')
            charging = {direct: ' · turbo charging', fast: ' · fast charging'}[
                fastCharging()] ?? ' · charging';
        this._battery.text = cap ? `${cap}%${charging}` : '';

        // a new spot every minute
        this._spot = [Math.random(), Math.random()];
        this._place();
    }

    /*
     * Overlay over the primary monitor, the clock box at this._spot (0..1
     * each way) of its area. Also on monitors-changed: the lock screen
     * turns a landscape panel back to portrait just after the AOD came up.
     */
    _place() {
        const m = Main.layoutManager.primaryMonitor;
        this._overlay.set_position(m.x, m.y);
        this._overlay.set_size(m.width, m.height);

        const [, bw] = this._box.get_preferred_width(-1);
        const [, bh] = this._box.get_preferred_height(bw);
        const along = (size, box, margin, at) => {
            const lo = size * margin, hi = size * (1 - margin) - box;
            const pos = hi >= lo ? lo + at * (hi - lo) : (size - box) / 2;
            return Math.round(Math.max(0, Math.min(size - box, pos)));
        };
        this._box.set_position(along(m.width, bw, AREA_X, this._spot[0]),
            along(m.height, bh, AREA_Y, this._spot[1]));
    }

    /*
     * The RTC and the system clock drift apart a little: woken for a
     * minute update the system may still be a moment before that minute;
     * show the alarm's minute then (not when woken earlier, by Wi-Fi).
     */
    _showTime() {
        const now = Math.floor(Date.now() / 1000);
        return this._alarm - now <= ALARM_SLACK ? Math.max(now, this._alarm) : now;
    }

    // fadeBlack: from the visible screen; else it starts black (blanked)
    _show(fadeBlack) {
        this._layout(this._showTime());
        if (!this._active) {
            this._overlay.opacity = fadeBlack ? 0 : 255;
            this._box.opacity = 0;
        }
        this._overlay.show();
        global.stage.set_child_above_sibling(this._overlay, null);
    }

    _fadeIn() {
        return new Promise(resolve => {
            const clock = () => this._box.ease({
                opacity: 255,
                duration: FADE_CLOCK_MS,
                mode: Clutter.AnimationMode.EASE_OUT_QUAD,
                onStopped: () => resolve(),
            });
            if (this._overlay.opacity === 255) {
                clock();
                return;
            }
            this._overlay.ease({
                opacity: 255,
                duration: FADE_BLACK_MS,
                mode: Clutter.AnimationMode.EASE_IN_QUAD,
                onStopped: clock,
            });
        });
    }

    _dozeLevel(fresh = false) {
        const mode = this._settings?.get_string('aod-brightness') ?? 'auto';
        if (mode !== 'auto')
            return mode === 'high' ? '2' : '1';
        const now = Date.now() / 1000;
        if (fresh || now - this._luxAt >= LUX_EVERY_S) {
            this._luxLevel = parseInt(readStr(LUX, '0')) > BRIGHT_LUX ? '2' : '1';
            this._luxAt = now;
        }
        return this._luxLevel;
    }

    /*
     * Wi-Fi in standby (cezanne-power): off in the AOD at once or after a
     * while, back on when the AOD ends. A radio the user turned off stays
     * off. Connected Wi-Fi costs no measurable power asleep (38 mA AOD
     * standby either way); the AOD itself is ~18 mA of it.
     */
    async _wifiStandby() {
        const mode = this._settings?.get_string('wifi-standby') ?? 'connected';
        if (mode === 'connected' || this._wifiOff || !this._aodSince)
            return;
        const after = mode === 'off' ? 0
            : this._settings.get_uint('wifi-standby-minutes') * 60;
        if (Date.now() / 1000 - this._aodSince < after)
            return;
        if (!await getProperty(Gio.DBus.system, NM, NM_PATH, NM, 'WirelessEnabled'))
            return;
        this._wifiOff = await setProperty(Gio.DBus.system, NM, NM_PATH, NM,
            'WirelessEnabled', new GLib.Variant('b', false));
    }

    _wifiResume() {
        this._aodSince = 0;
        if (!this._wifiOff)
            return;
        this._wifiOff = false;
        setProperty(Gio.DBus.system, NM, NM_PATH, NM, 'WirelessEnabled',
            new GLib.Variant('b', true));
    }

    /*
     * Bluetooth in standby (cezanne-power): off while the phone sleeps
     * unless a device is connected (a headset), on again when the AOD ends.
     * On and idle (a paired device: page scans) it costs ~1 mA asleep.
     */
    async _btStandby() {
        if (!(this._settings?.get_boolean('bluetooth-standby') ?? true) || this._btOff)
            return;
        if (!await getProperty(Gio.DBus.system, BLUEZ, BT_ADAPTER,
            'org.bluez.Adapter1', 'Powered'))
            return;
        if (await btConnected())
            return;
        this._btOff = await setProperty(Gio.DBus.system, BLUEZ, BT_ADAPTER,
            'org.bluez.Adapter1', 'Powered', new GLib.Variant('b', false));
    }

    _btResume() {
        if (!this._btOff)
            return;
        this._btOff = false;
        setProperty(Gio.DBus.system, BLUEZ, BT_ADAPTER, 'org.bluez.Adapter1',
            'Powered', new GLib.Variant('b', true));
    }

    // shown: the overlay is already up (_show)
    async _enter(shown = false) {
        const t = this._showTime();
        if (!shown)
            this._show(true);
        await this._fadeIn();

        // once the overlay frame is out: doze, alarm, then let it sleep
        await this._framePresented();
        const level = this._dozeLevel(true);
        let dozing = await writeStrAsync(DOZE, level);
        if (!dozing) {
            /*
             * The shell blanks the screen on suspend (lock screen), and
             * the fade in can lose that race: the panel is off then. Turn
             * it back on with the overlay up and try again.
             */
            await this._powerSave(0);
            await this._framePresented();
            dozing = await writeStrAsync(DOZE, level);
        }
        this._active = dozing;
        if (dozing && !this._aodSince)
            this._aodSince = Date.now() / 1000;
        if (dozing)
            this._holdIdle();
        // behind the overlay: a wake comes back to the clock page, not to a
        // password prompt left open (cezanne-fod keeps the prompt alive).
        // At once: the crossfade doesn't run while the display is blanked
        const dialog = Main.screenShield._dialog;
        if (dialog?._showClock && dialog._adjustment) {
            dialog._showClock();
            dialog._adjustment.remove_transition('value');
            dialog._adjustment.value = 0;
        }
        this._arm(t);
    }

    // in the AOD, awake: a new minute moves the clock, then the next alarm
    async _update() {
        const t = this._showTime();
        if (Math.floor(t / 60) !== this._minute) {
            await this._displayOn();
            if (!this._active)
                return;
            this._show(false);
            await this._framePresented();
            if (!this._active)
                return;
            await writeStrAsync(DOZE, this._dozeLevel());
        }
        this._arm(t);
    }

    // RTC alarm 1 s into the minute after t, at least 2 s ahead
    _arm(t) {
        const now = Math.floor(Date.now() / 1000);
        let alarm = t - t % 60 + 60 + 1;
        if (alarm < now + 2)
            alarm += 60;
        if (alarm === this._alarm)
            return;		// still armed (woken by something else)
        this._alarm = alarm;
        writeStr(WAKEALARM, '0');
        writeStr(WAKEALARM, `${alarm}`);
    }

    // resolves 50 ms after the next stage frame (the overlay) is out
    // (or after 1 s: no paint while the display is off)
    _framePresented() {
        return new Promise(resolve => {
            let timeoutId = 0;
            const id = global.stage.connect('after-paint', () => {
                global.stage.disconnect(id);
                GLib.source_remove(timeoutId);
                GLib.timeout_add(GLib.PRIORITY_DEFAULT, 50, () => {
                    resolve();
                    return GLib.SOURCE_REMOVE;
                });
            });
            timeoutId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1000, () => {
                global.stage.disconnect(id);
                resolve();
                return GLib.SOURCE_REMOVE;
            });
            global.stage.queue_redraw();
        });
    }

    /*
     * gsd-power blanks the display when the lock screen's fade ends, under
     * the AOD (the panel keeps showing its frame memory). A logind resume
     * turned it back on; these sleeps don't: on again before new frames
     * and before leaving doze (DSI off: the doze commands fail, EAGAIN).
     */
    async _displayOn() {
        if (await this._powerSave() > 0)
            await this._powerSave(0);
    }

    _exit() {
        if (!this._overlay?.visible && !this._active)
            return;
        this._alarm = 0;
        writeStr(WAKEALARM, '0');
        this._releaseIdle();
        this._wifiResume();
        this._btResume();
        this._overlay.hide();
        if (this._active) {
            this._active = false;
            this._leaveDoze();
        }
    }

    // once the lock screen frame is out; a doze left on keeps it dim and
    // blocks brightness / HBM (the fingerprint spot)
    async _leaveDoze() {
        await this._displayOn();
        for (let i = 0; i < 3 && !this._active; i++) {
            await this._framePresented();
            if (this._active || await writeStrAsync(DOZE, '0'))
                return;
            await this._displayOn();
        }
    }

    /*
     * Awake between sleeps the session's idle time grows (a logind resume
     * resets it in mutter, these sleeps don't): keep gsd-power from
     * dimming or blanking the AOD.
     */
    _holdIdle() {
        if (this._idleCookie !== null)
            return;
        this._idleCookie = 0;
        Gio.DBus.session.call(SESSION_MANAGER, SESSION_MANAGER_PATH,
            SESSION_MANAGER, 'Inhibit', new GLib.Variant('(susu)',
                ['org.gnome.Shell', 0, 'Always-on display', INHIBIT_IDLE]),
            new GLib.VariantType('(u)'), Gio.DBusCallFlags.NONE, -1, null,
            (conn, res) => {
                let cookie = 0;
                try {
                    [cookie] = conn.call_finish(res).deepUnpack();
                } catch (e) {
                    console.log(`cezanne-aod: idle inhibit: ${e.message}`);
                }
                if (this._idleCookie === 0 && cookie)
                    this._idleCookie = cookie;
                else if (cookie)
                    this._uninhibit(cookie);	// released meanwhile
            });
    }

    _releaseIdle() {
        const cookie = this._idleCookie;
        this._idleCookie = null;
        if (cookie)
            this._uninhibit(cookie);
    }

    _uninhibit(cookie) {
        Gio.DBus.session.call(SESSION_MANAGER, SESSION_MANAGER_PATH,
            SESSION_MANAGER, 'Uninhibit', new GLib.Variant('(u)', [cookie]),
            null, Gio.DBusCallFlags.NONE, -1, null, (conn, res) => {
                try {
                    conn.call_finish(res);
                } catch (e) {
                    console.log(`cezanne-aod: idle uninhibit: ${e.message}`);
                }
            });
    }

    _suspend() {
        Gio.DBus.system.call('org.freedesktop.login1',
            '/org/freedesktop/login1', 'org.freedesktop.login1.Manager',
            'Suspend', new GLib.Variant('(b)', [false]), null,
            Gio.DBusCallFlags.NONE, -1, null, (conn, res) => {
                try {
                    conn.call_finish(res);
                } catch (e) {
                    console.log(`cezanne-aod: suspend: ${e.message}`);
                    this._exit();
                }
            });
    }
}
