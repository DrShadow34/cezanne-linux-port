// SPDX-License-Identifier: GPL-2.0-or-later
// Quick Settings NFC toggle and tag notifications, as Android's NFC switch:
// while NFC is on and the session is unlocked, neard (org.neard on the
// system bus, D-Bus activated) polls as reader on the first adapter
// (/org/neard/nfcN; nxp-nci on the SN100U). The extension stays enabled on
// the lock screen (the toggle is in its quick settings too, and nothing is
// rebuilt on unlock) but turns the field off while locked; the screen
// blanks only once locked, so this is also what keeps NFC from costing
// battery.
//
// A tag or card in the field gives a notification: its kind and UID, the
// NDEF records neard read from it, and "Open" for a URI record. neard
// re-detects a tag it could not read (bank cards: no NDEF) every poll
// cycle, so the same UID is reported once until it has been gone a while.
//
// No notification while the NFC app (org.cezanne.Nfc, apps/cezanne-nfc) has
// the focus. The on / off choice is the extension's 'enabled' setting
// (org.gnome.shell.extensions.cezanne-nfc).

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as MessageTray from 'resource:///org/gnome/shell/ui/messageTray.js';
import {QuickToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';

const BUS_NAME = 'org.neard';
// the NFC app shows tags itself while it has the focus
const APP_ID = 'org.cezanne.Nfc';
const ADAPTER_IFACE = 'org.neard.Adapter';
const TAG_IFACE = 'org.neard.Tag';
const DEVICE_IFACE = 'org.neard.Device';
const RECORD_IFACE = 'org.neard.Record';
const PROPS_IFACE = 'org.freedesktop.DBus.Properties';
const OM_IFACE = 'org.freedesktop.DBus.ObjectManager';
// records of a tag come in after the tag itself
const RECORD_WAIT_MS = 700;
// a UID not seen for this long counts as a new tap
const FORGET_MS = 4000;
// neard restarts polling after a tag leaves (ConstantPoll); this catches
// a poll loop that stopped on an error
const CHECK_SECONDS = 10;

const TAG_TYPES = {
    'Type 1': 'NFC Forum Type 1 tag (Topaz)',
    'Type 2': 'NFC Forum Type 2 tag (NTAG / MIFARE Ultralight)',
    'Type 3': 'NFC Forum Type 3 tag (FeliCa)',
    'Type 4': 'NFC Forum Type 4 tag (ISO-DEP)',
    'Type 4A': 'NFC Forum Type 4A tag (ISO-DEP)',
    'Type 4B': 'NFC Forum Type 4B tag (ISO-DEP)',
    'Type 5': 'NFC Forum Type 5 tag (ISO 15693)',
};

function hex(bytes) {
    return Array.from(bytes, b => b.toString(16).padStart(2, '0').toUpperCase()).join(':');
}

const NfcToggle = GObject.registerClass(
class NfcToggle extends QuickToggle {
    constructor(gicon) {
        super({title: 'NFC', gicon, toggleMode: true});
    }
});

const NfcIndicator = GObject.registerClass(
class NfcIndicator extends SystemIndicator {
    constructor(extension) {
        super();

        this._gicon = Gio.icon_new_for_string(`${extension.path}/icons/nfc-symbolic.svg`);
        this._settings = extension.getSettings();
        this._adapter = null;
        this._polling = false;
        this._seen = new Map();     // uid -> time last seen
        this._pending = new Map();  // tag path -> {props, records, timeout}
        this._signals = [];
        this._cancellable = new Gio.Cancellable();

        this._indicator = this._addIndicator();
        this._indicator.gicon = this._gicon;
        this._toggle = new NfcToggle(this._gicon);
        this._settings.bind('enabled', this._toggle, 'checked', Gio.SettingsBindFlags.DEFAULT);
        this._toggle.bind_property('checked', this._indicator, 'visible',
            GObject.BindingFlags.SYNC_CREATE);
        this._toggle.connect('notify::checked', () => this._apply());
        this.quickSettingsItems.push(this._toggle);
        this._updateSubtitle();
        Main.sessionMode.connectObject('updated', () => this._apply(), this);

        this._bus = Gio.DBus.system;
        this._watch = Gio.bus_watch_name(Gio.BusType.SYSTEM, BUS_NAME,
            Gio.BusNameWatcherFlags.AUTO_START,
            () => this._appeared(), () => this._vanished());
        this._check = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, CHECK_SECONDS, () => {
            if (this._reading() && this._adapter && !this._polling && this._pending.size === 0)
                this._startPoll();
            return GLib.SOURCE_CONTINUE;
        });
    }

    _call(path, iface, method, args) {
        return new Promise((resolve, reject) => {
            this._bus.call(BUS_NAME, path, iface, method, args, null,
                Gio.DBusCallFlags.NONE, 5000, this._cancellable, (bus, res) => {
                    try {
                        resolve(bus.call_finish(res));
                    } catch (e) {
                        reject(e);
                    }
                });
        });
    }

    async _appeared() {
        for (const [iface, signal, handler] of [
            [OM_IFACE, 'InterfacesAdded', (...a) => this._added(...a)],
            [OM_IFACE, 'InterfacesRemoved', (...a) => this._removed(...a)],
            [PROPS_IFACE, 'PropertiesChanged', (...a) => this._changed(...a)],
        ]) {
            this._signals.push(this._bus.signal_subscribe(BUS_NAME, iface, signal,
                null, null, Gio.DBusSignalFlags.NONE,
                (c, s, path, i, n, params) => handler(path, params)));
        }
        try {
            const [objects] = (await this._call('/', OM_IFACE, 'GetManagedObjects', null)).deepUnpack();
            for (const [path, ifaces] of Object.entries(objects)) {
                if (ADAPTER_IFACE in ifaces && !this._adapter)
                    this._setAdapter(path, ifaces[ADAPTER_IFACE]);
            }
        } catch (e) {
            if (!e.matches?.(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                console.warn(`NFC: neard: ${e.message}`);
        }
    }

    _vanished() {
        for (const id of this._signals)
            this._bus.signal_unsubscribe(id);
        this._signals = [];
        this._adapter = null;
        this._polling = false;
        this._updateSubtitle();
    }

    _setAdapter(path, props) {
        this._adapter = path;
        this._polling = props.Polling?.deepUnpack() ?? false;
        this._updateSubtitle();
        this._apply();
    }

    // on and unlocked: the field is up
    _reading() {
        return this._toggle.checked && !Main.sessionMode.isLocked;
    }

    _updateSubtitle() {
        this._toggle.subtitle = !this._adapter ? 'Unavailable'
            : !this._toggle.checked ? null
            : Main.sessionMode.isLocked ? 'Off while locked' : 'Reading tags';
    }

    async _setPowered(on) {
        try {
            await this._call(this._adapter, PROPS_IFACE, 'Set',
                new GLib.Variant('(ssv)', [ADAPTER_IFACE, 'Powered', new GLib.Variant('b', on)]));
        } catch (e) {
            // "Device already enabled" / "already disabled"
            if (!/already/i.test(e.message))
                throw e;
        }
    }

    async _startPoll() {
        try {
            await this._call(this._adapter, ADAPTER_IFACE, 'StartPollLoop',
                new GLib.Variant('(s)', ['Initiator']));
        } catch (e) {
            if (!/busy/i.test(e.message))  // already polling
                throw e;
        }
    }

    async _apply() {
        this._updateSubtitle();
        if (!this._adapter)
            return;
        const on = this._reading();
        try {
            if (on) {
                await this._setPowered(true);
                await this._startPoll();
            } else {
                await this._call(this._adapter, ADAPTER_IFACE, 'StopPollLoop', null).catch(() => {});
                await this._setPowered(false);
            }
            console.log(`NFC: ${on ? 'on' : 'off'}`);
        } catch (e) {
            if (!e.matches?.(Gio.IOErrorEnum, Gio.IOErrorEnum.CANCELLED))
                console.warn(`NFC: ${on ? 'on' : 'off'}: ${e.message}`);
        }
    }

    _changed(path, params) {
        const [iface, changed] = params.deepUnpack();
        if (path === this._adapter && iface === ADAPTER_IFACE && 'Polling' in changed)
            this._polling = changed.Polling.deepUnpack();
    }

    _added(_path, params) {
        const [path, ifaces] = params.deepUnpack();
        if (ADAPTER_IFACE in ifaces && !this._adapter) {
            this._setAdapter(path, ifaces[ADAPTER_IFACE]);
        } else if (TAG_IFACE in ifaces || DEVICE_IFACE in ifaces) {
            const tag = TAG_IFACE in ifaces;
            const props = Object.fromEntries(Object.entries(ifaces[tag ? TAG_IFACE : DEVICE_IFACE])
                .map(([k, v]) => [k, v.deepUnpack()]));
            const pending = {tag, props, records: []};
            pending.timeout = GLib.timeout_add(GLib.PRIORITY_DEFAULT, RECORD_WAIT_MS, () => {
                this._pending.delete(path);
                this._report(pending);
                return GLib.SOURCE_REMOVE;
            });
            this._pending.set(path, pending);
        } else if (RECORD_IFACE in ifaces) {
            const parent = this._pending.get(path.substring(0, path.lastIndexOf('/')));
            parent?.records.push(Object.fromEntries(Object.entries(ifaces[RECORD_IFACE])
                .map(([k, v]) => [k, v.deepUnpack()])));
        }
    }

    _removed(_path, params) {
        const [path, ifaces] = params.deepUnpack();
        if (ifaces.includes(ADAPTER_IFACE) && path === this._adapter) {
            this._adapter = null;
            this._updateSubtitle();
        }
    }

    _report({tag, props, records}) {
        const now = GLib.get_monotonic_time() / 1000;
        const uid = props.Uid?.length ? hex(props.Uid) : null;
        const key = uid ?? (tag ? `${props.Type}/${props.Protocol}` : 'device');
        for (const [k, t] of this._seen) {
            if (now - t > FORGET_MS)
                this._seen.delete(k);
        }
        const known = this._seen.has(key);
        this._seen.set(key, now);
        // the same tag again in one sitting; a re-read with records still counts
        if (known && records.length === 0)
            return;

        let title, body = [];
        if (!tag) {
            title = 'NFC device nearby';
        } else if (records.length === 0 && props.Protocol === 'ISO-DEP') {
            title = 'Contactless card';
            body.push('ISO-DEP card without NFC data (payment, transit or ID card)');
        } else {
            title = records.length ? 'NFC tag' : 'Empty NFC tag';
            body.push(TAG_TYPES[props.Type] ?? `${props.Type ?? 'Unknown'} tag (${props.Protocol})`);
        }
        if (uid)
            body.push(`UID ${uid}`);
        let uri = null;
        for (const r of records) {
            if (r.URI) {
                uri ??= r.URI;
                body.push(r.URI);
            } else if (r.Representation) {
                body.push(r.Representation);
            } else if (r.MIME || r.MIMEType) {
                body.push(`${r.MIME ?? r.MIMEType} data`);
            } else if (r.Type) {
                body.push(`${r.Type} record`);
            }
        }
        if (tag && props.ReadOnly && records.length)
            body.push('Read-only');
        console.log(`NFC: ${title}: ${body.join(', ')}`);
        if (global.display.focus_window?.get_gtk_application_id() !== APP_ID)
            this._notify(title, body.join('\n'), uri, records);
    }

    _notify(title, body, uri, records) {
        if (!this._source) {
            this._source = new MessageTray.Source({title: 'NFC', icon: this._gicon});
            this._source.connect('destroy', () => (this._source = null));
            Main.messageTray.add(this._source);
        }
        const notification = new MessageTray.Notification({
            source: this._source, title, body, gicon: this._gicon,
        });
        if (uri) {
            notification.addAction('Open', () => {
                Gio.AppInfo.launch_default_for_uri_async(uri, null, null, (o, res) => {
                    try {
                        Gio.AppInfo.launch_default_for_uri_finish(res);
                    } catch (e) {
                        console.warn(`NFC: open ${uri}: ${e.message}`);
                    }
                });
            });
        }
        const text = records.map(r => r.URI ?? r.Representation).filter(Boolean).join('\n');
        if (text) {
            notification.addAction('Copy', () =>
                St.Clipboard.get_default().set_text(St.ClipboardType.CLIPBOARD, text));
        }
        this._source.addNotification(notification);
    }

    destroy() {
        Main.sessionMode.disconnectObject(this);
        GLib.source_remove(this._check);
        for (const p of this._pending.values())
            GLib.source_remove(p.timeout);
        this._pending.clear();
        // disabled: stop the field without touching the user's setting
        if (this._adapter) {
            const adapter = this._adapter;
            this._bus.call(BUS_NAME, adapter, ADAPTER_IFACE, 'StopPollLoop', null, null,
                Gio.DBusCallFlags.NONE, 5000, null, () => {
                    this._bus.call(BUS_NAME, adapter, PROPS_IFACE, 'Set',
                        new GLib.Variant('(ssv)', [ADAPTER_IFACE, 'Powered', new GLib.Variant('b', false)]),
                        null, Gio.DBusCallFlags.NONE, 5000, null, null);
                });
        }
        this._cancellable.cancel();
        Gio.bus_unwatch_name(this._watch);
        for (const id of this._signals)
            this._bus.signal_unsubscribe(id);
        this._signals = [];
        this._source?.destroy();
        this._source = null;
        this.quickSettingsItems.forEach(item => item.destroy());
        super.destroy();
    }
});

export default class NfcExtension extends Extension {
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
        this._indicator = new NfcIndicator(this);
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    _disable() {
        this._indicator?.destroy();
        this._indicator = null;
    }
}
