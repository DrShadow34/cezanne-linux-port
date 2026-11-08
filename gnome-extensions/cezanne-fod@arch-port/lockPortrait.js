// The lock screen (and the AOD) stays in portrait, whatever auto-rotation
// says: the fingerprint sensor sits at a fixed spot of the panel, and the
// spot's icon / press highlight only line up with it unrotated (in
// landscape the fingerprint did not unlock).
//
// Locked: the monitor manager's handler of Meta.OrientationManager
// 'orientation-changed' (the only one, mutter's auto-rotation) is blocked,
// the user's orientation-lock setting stays as it is, and the panel goes to
// transform 0 (DisplayConfig ApplyMonitorsConfig, temporary).
// (inhibit_tracking() would be the API, but libmutter-51 does not export
// it.) Unlocked: unblocked, and the rotation the sensor reports now
// (auto-rotation on; the manager kept tracking) or the one before the lock
// (off) is set here: mutter only rotates on the next change. With more than
// one display (docked) nothing is touched. Locked, every monitors-changed
// puts transform 0 back: a configuration mutter builds anew (resumes in the
// AOD) takes the panel's rotation from the sensor directly.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const DISPLAY_CONFIG = 'org.gnome.Mutter.DisplayConfig';
const DISPLAY_CONFIG_PATH = '/org/gnome/Mutter/DisplayConfig';
const TOUCHSCREEN_SCHEMA = 'org.gnome.settings-daemon.peripherals.touchscreen';
const METHOD_TEMPORARY = 1;

// MetaOrientation -> logical monitor transform (meta_orientation_to_transform)
const TRANSFORMS = new Map([
    [Meta.Orientation.NORMAL, 0],
    [Meta.Orientation.LEFT_UP, 1],
    [Meta.Orientation.BOTTOM_UP, 2],
    [Meta.Orientation.RIGHT_UP, 3],
]);

function displayConfig(method, params) {
    return new Promise((resolve, reject) => {
        Gio.DBus.session.call(DISPLAY_CONFIG, DISPLAY_CONFIG_PATH, DISPLAY_CONFIG,
            method, params, null, Gio.DBusCallFlags.NONE, -1, null, (conn, res) => {
                try {
                    resolve(conn.call_finish(res));
                } catch (e) {
                    reject(e);
                }
            });
    });
}

// the single logical monitor: serial, its fields, the current mode of each
// of its monitors; null with more than one
async function currentState() {
    const [serial, monitors, logical] =
        (await displayConfig('GetCurrentState', null)).deepUnpack();
    if (logical.length !== 1)
        return null;
    const modes = new Map();
    for (const [[connector], monitorModes] of monitors) {
        const current = monitorModes.find(m => m[6]['is-current']?.unpack());
        if (current)
            modes.set(connector, current[0]);
    }
    const [x, y, scale, transform, primary, connectors] = logical[0];
    return {serial, x, y, scale, transform, primary, connectors, modes};
}

async function setTransform(transform) {
    const st = await currentState();
    if (!st || st.transform === transform)
        return;
    const monitors = st.connectors.map(([connector]) =>
        [connector, st.modes.get(connector), {}]);
    await displayConfig('ApplyMonitorsConfig',
        new GLib.Variant('(uua(iiduba(ssa{sv}))a{sv})', [st.serial, METHOD_TEMPORARY,
            [[st.x, st.y, st.scale, transform, st.primary, monitors]], {}]));
}

export class LockPortrait {
    constructor() {
        this._orientation = global.backend.get_orientation_manager();
        this._settings = new Gio.Settings({schema_id: TOUCHSCREEN_SCHEMA});
        this._blocked = false;
        this._saved = null;	// transform before the lock
        Main.sessionMode.connectObject('updated', () => this._sync(), this);
        // a configuration mutter builds anew (resumes in the AOD) takes the
        // panel's rotation from the sensor, not through the blocked signal
        Main.layoutManager.connectObject('monitors-changed', () => {
            if (this._blocked)
                setTransform(0).catch(e => console.log(`cezanne-fod: lock screen rotation: ${e.message}`));
        }, this);
        this._sync();
    }

    _sync() {
        (Main.sessionMode.isLocked ? this._lock() : this._unlock())
            .catch(e => console.log(`cezanne-fod: lock screen rotation: ${e.message}`));
    }

    _block(block) {
        if (block === this._blocked)
            return;
        (block ? GObject.signal_handlers_block_matched
            : GObject.signal_handlers_unblock_matched)(this._orientation,
            {signalId: 'orientation-changed'});	// GJS takes the name
        this._blocked = block;
    }

    async _lock() {
        if (this._blocked)
            return;
        this._block(true);
        const st = await currentState();
        if (!st || !this._blocked)
            return;
        this._saved = st.transform;
        await setTransform(0);
    }

    async _unlock() {
        if (!this._blocked)
            return;
        this._block(false);
        const saved = this._saved;
        this._saved = null;
        if (saved === null)
            return;
        let target = saved;
        if (!this._settings.get_boolean('orientation-lock'))
            target = TRANSFORMS.get(this._orientation.get_orientation()) ?? saved;
        await setTransform(target);
    }

    destroy() {
        Main.sessionMode.disconnectObject(this);
        Main.layoutManager.disconnectObject(this);
        this._block(false);
    }
}
