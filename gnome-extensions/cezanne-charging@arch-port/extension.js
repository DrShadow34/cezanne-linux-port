// Charging power next to the battery icon (GNOME only shows charging or not).
//
// While the battery charges, the top bar shows the power going into it
// (battery voltage x current from the MT6359 gauge), with a bolt when the
// USB PD contract or a QC charger (xmusb350 qc_voltage) is above 5 V ("fast
// charging": 9 V into the MT6360 charger, see rootfs mt6360-chg-setup /
// cezanne-dcharge) and a yellow bolt while the BQ25970
// charge pump runs ("turbo": PPS direct charging, rootfs cezanne-dcharge).
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';
import Clutter from 'gi://Clutter';
import Pango from 'gi://Pango';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const PS = '/sys/class/power_supply';
const BATTERY = `${PS}/battery`;
const POLL_CHARGING_S = 3;
const POLL_IDLE_S = 10;
const TURBO = '#f8e45c';     // GNOME palette yellow 2

Gio._promisify(Gio.File.prototype, 'load_contents_async');

// Asynchronous: a power supply property can block for seconds (an I2C
// device timing out), which must not freeze the shell.
async function readStr(path) {
    try {
        const [b] = await Gio.File.new_for_path(path).load_contents_async(null);
        return new TextDecoder().decode(b).trim();
    } catch {
        return '';
    }
}

// USB PD contract: {mv, ma, pps} of the online TCPM supply, or null
async function pdContract() {
    let dir;
    try {
        dir = GLib.Dir.open(PS, 0);
    } catch {
        return null;
    }
    const names = [];
    let name;
    while ((name = dir.read_name()) !== null) {
        if (name.startsWith('tcpm-source-psy-'))
            names.push(name);
    }
    dir.close();

    let contract = null;
    for (name of names) {
        // online 1 = fixed PDO, 2 = PPS
        const online = await readStr(`${PS}/${name}/online`);
        if (online === '0' || online === '')
            continue;
        contract = {
            mv: parseInt(await readStr(`${PS}/${name}/voltage_now`)) / 1000,
            ma: parseInt(await readStr(`${PS}/${name}/current_max`)) / 1000,
            pps: online === '2',
        };
    }
    return contract;
}

export default class CezanneCharging extends Extension {
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
        // not an icon: own natural width, no ellipsis
        this._label = new St.Label({
            // compact: the right of the 540 px portrait panel has ~190 px
            style: 'font-feature-settings: "tnum"; font-size: 0.9em; padding: 0 1px 0 0;',
            y_align: Clutter.ActorAlign.CENTER,
            visible: false,
        });
        this._label.clutter_text.ellipsize = Pango.EllipsizeMode.NONE;
        // before the battery (system) indicator
        const qs = Main.panel.statusArea.quickSettings;
        if (qs._system)
            qs._indicators.insert_child_below(this._label, qs._system);
        else
            qs._indicators.add_child(this._label);

        this._update().catch(logError);
    }

    _disable() {
        if (this._timeout)
            GLib.source_remove(this._timeout);
        this._timeout = 0;
        this._label.destroy();
        this._label = null;
    }

    _schedule(seconds) {
        this._timeout = GLib.timeout_add_seconds(GLib.PRIORITY_LOW, seconds, () => {
            this._timeout = 0;
            this._update().catch(logError);
            return GLib.SOURCE_REMOVE;
        });
    }

    async _update() {
        const status = await readStr(`${BATTERY}/status`);
        const ua = parseInt(await readStr(`${BATTERY}/current_now`));
        const uv = parseInt(await readStr(`${BATTERY}/voltage_now`));
        const charging = status === 'Charging' && ua > 0;

        // disabled while waiting
        if (!this._label)
            return;

        if (!charging) {
            this._label.visible = false;
            this._schedule(POLL_IDLE_S);
            return;
        }

        const watts = ua / 1e6 * (uv / 1e6);
        const pd = await pdContract();
        // the charge pump only runs on a PPS contract: do not wake it up
        const direct = pd?.pps &&
            await readStr(`${PS}/bq25970/status`) === 'Charging';
        if (!this._label)
            return;
        const qcMv = parseInt(await readStr(`${PS}/xmusb350/qc_voltage`)) || 0;
        if (!this._label)
            return;
        const fast = (pd && pd.mv > 5500) || qcMv > 5500;
        const w = `${Math.round(watts)}W`;
        if (direct)
            this._label.clutter_text.set_markup(`<span foreground="${TURBO}">⚡</span>${w}`);
        else
            this._label.text = `${fast ? '⚡' : ''}${w}`;
        this._label.visible = true;
        this._schedule(POLL_CHARGING_S);
    }
}
