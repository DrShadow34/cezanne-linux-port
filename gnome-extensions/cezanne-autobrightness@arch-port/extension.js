// Replaces gnome-settings-daemon's ambient brightness for the phone.
//
// gsd maps lux linearly (relative to the level when auto brightness was
// switched on) through a 0.1 Hz low-pass filter, and the shell adds the
// slider as an offset: target + slider - 0.5. On a phone that means 3-5 s
// lag and a slider at max pinning the screen to 100% even in the dark.
//
// Here: our own LightLevel subscription, a lux -> nits curve interpolated
// in log(lux) (see CURVE), brighten after 0.3 s / dim after 2 s,
// smooth ramps, and the slider scales the curve (x0.25 .. x4) instead of
// shifting it. gsd's SetAutoBrightnessTarget calls are swallowed; the
// ambient-enabled key (quick settings toggle) still switches it on/off.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const POWER_SCHEMA = 'org.gnome.settings-daemon.plugins.power';

// Panel nits at backlight 2047 (non-HBM); above ~1000 lux the
// sunlight-boost service switches the panel to HBM on top of this.
const MAX_NITS = 500;
// The front ALS reads low: 0-2 lux in a lamp-lit room, ~7 lux indoors by
// day. MIUI's curve (vendor FrameworkResOverlayExt,
// config_autoBrightnessLevels / DisplayValuesNits) gives 5 nits there, too
// dim; this one is ~3x brighter below 30 lux and meets it in sunlight.
const CURVE = [		// [lux, nits]
    [0, 10], [2, 22], [5, 40], [10, 65], [30, 110], [80, 160], [200, 230],
    [500, 320], [1000, 410], [2000, 470], [4000, 500],
];
const GAIN_RANGE = 4;		// slider 0 -> /4, 0.5 -> x1, 1 -> x4
const UP_DELAY_MS = 300;
const DOWN_DELAY_MS = 2000;
const UP_TAU_MS = 250;
const DOWN_TAU_MS = 1500;
const STEP_MS = 33;
const HYST = 0.10;		// ignore lux changes under 10% (+1 lux)

const SensorIface = `<node><interface name="net.hadess.SensorProxy">
  <method name="ClaimLight"/><method name="ReleaseLight"/>
  <property name="HasAmbientLight" type="b" access="read"/>
  <property name="LightLevel" type="d" access="read"/>
</interface></node>`;
const SensorProxy = Gio.DBusProxy.makeProxyWrapper(SensorIface);

function luxToLevel(lux) {
    const f = l => Math.log10(1 + l);
    const x = f(Math.max(0, lux));
    let nits = CURVE[CURVE.length - 1][1];
    for (let i = 1; i < CURVE.length; i++) {
        const [l0, n0] = CURVE[i - 1], [l1, n1] = CURVE[i];
        if (x <= f(l1)) {
            const t = (x - f(l0)) / (f(l1) - f(l0));
            nits = n0 + t * (n1 - n0);
            break;
        }
    }
    return nits / MAX_NITS;
}

export default class CezanneAutoBrightness extends Extension {
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
        this._mgr = Main.brightnessManager;
        this._dbus = Main.brightnessDBus;
        this._settings = new Gio.Settings({schema_id: POWER_SCHEMA});
        this._lux = -1;
        this._goal = -1;
        this._level = -1;

        // swallow gsd's targets; we drive the auto target ourselves
        this._origSetTarget = this._dbus.SetAutoBrightnessTarget;
        this._dbus.SetAutoBrightnessTarget = () => {};

        // slider as a gain on the auto level instead of an offset
        const scale = this._mgr._monitorScales.values().next().value;
        this._scaleProto = scale ? Object.getPrototypeOf(scale) : null;
        if (this._scaleProto) {
            this._origSetBacklight = this._scaleProto.setBacklight;
            const orig = this._origSetBacklight;
            const mgr = this._mgr;
            this._scaleProto.setBacklight = function (brightness) {
                if (mgr._abTarget >= 0) {
                    const gain = GAIN_RANGE ** (2 * this.value - 1);
                    brightness = Math.clamp(mgr._abTarget * gain, 0, 1);
                    if (mgr._dimmingEnabled)
                        brightness = Math.min(brightness, mgr._dimmingTarget);
                }
                return orig.call(this, brightness);
            };
            // mutter's backlight readbacks lag our ramp; the shell would
            // take them for external changes and move the slider to them
            this._origSyncWithBacklight = this._scaleProto.syncWithBacklight;
            const origSync = this._origSyncWithBacklight;
            this._scaleProto.syncWithBacklight = function () {
                return mgr._abTarget >= 0 ? false : origSync.call(this);
            };
        }

        this._settings.connectObject('changed::ambient-enabled',
            () => this._syncEnabled(), this);
        this._syncEnabled();
    }

    _disable() {
        this._settings.disconnectObject(this);
        this._stop();
        if (this._scaleProto) {
            this._scaleProto.setBacklight = this._origSetBacklight;
            this._scaleProto.syncWithBacklight = this._origSyncWithBacklight;
        }
        this._dbus.SetAutoBrightnessTarget = this._origSetTarget;
        // hand back to gsd: it resends a target on the next lux change
        this._mgr.autoBrightnessTarget = -1;
        this._settings = null;
        this._mgr = null;
        this._dbus = null;
    }

    _syncEnabled() {
        if (this._settings.get_boolean('ambient-enabled'))
            this._start();
        else
            this._stop();
    }

    _start() {
        if (this._proxy)
            return;
        this._proxy = new SensorProxy(Gio.DBus.system,
            'net.hadess.SensorProxy', '/net/hadess/SensorProxy',
            (proxy, error) => {
                if (error) {
                    console.log(`cezanne-autobrightness: ${error.message}`);
                    return;
                }
                // LightLevel only changes on new readings: apply the
                // current one right away
                proxy.ClaimLightAsync()
                    .then(() => this._proxy === proxy &&
                        this._onLux(proxy.LightLevel))
                    .catch(e => console.log(e.message));
            });
        this._proxy.connectObject('g-properties-changed',
            () => this._onLux(this._proxy.LightLevel), this);
    }

    _stop() {
        if (this._proxy) {
            this._proxy.disconnectObject(this);
            this._proxy.ReleaseLightAsync().catch(() => {});
            this._proxy = null;
        }
        this._clearTimers();
        this._lux = this._goal = this._level = -1;
        if (this._mgr && this._mgr.autoBrightnessTarget >= 0)
            this._mgr.autoBrightnessTarget = -1;
    }

    _clearTimers() {
        if (this._delayId)
            GLib.source_remove(this._delayId);
        if (this._rampId)
            GLib.source_remove(this._rampId);
        this._delayId = this._rampId = 0;
    }

    _onLux(lux) {
        if (lux === undefined || lux === null)
            return;
        if (this._lux >= 0 && Math.abs(lux - this._lux) <= HYST * this._lux + 1)
            return;
        this._lux = lux;
        const goal = luxToLevel(lux);

        if (this._level < 0) {		// first reading: jump
            this._level = this._goal = goal;
            this._apply();
            return;
        }
        if (this._delayId)
            GLib.source_remove(this._delayId);
        const up = goal > this._level;
        this._delayId = GLib.timeout_add(GLib.PRIORITY_DEFAULT,
            up ? UP_DELAY_MS : DOWN_DELAY_MS, () => {
                this._delayId = 0;
                this._goal = goal;
                this._ramp();
                return GLib.SOURCE_REMOVE;
            });
    }

    _ramp() {
        if (this._rampId)
            return;
        this._rampId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, STEP_MS, () => {
            const tau = this._goal > this._level ? UP_TAU_MS : DOWN_TAU_MS;
            const a = 1 - Math.exp(-STEP_MS / tau);
            this._level += (this._goal - this._level) * a;
            if (Math.abs(this._goal - this._level) < 0.002)
                this._level = this._goal;
            this._apply();
            if (this._level === this._goal) {
                this._rampId = 0;
                return GLib.SOURCE_REMOVE;
            }
            return GLib.SOURCE_CONTINUE;
        });
    }

    _apply() {
        this._mgr.autoBrightnessTarget = this._level;
    }
}
