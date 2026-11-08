// SPDX-License-Identifier: GPL-2.0-or-later
// "Use USB for", as on a phone: when a computer (not a charger) enumerates
// the USB-C port, ask whether it is for
//   Video stream       the phone as a UVC webcam (OBS etc.): the screen or
//                      a camera, picked on a second page
//   Data storage       MTP: the home folder
//   Developer network  RNDIS, the phone at 172.16.42.1 (ssh)
//   Charge only        no data (also Escape / dismissing)
// The dialog only comes up unlocked with nothing fullscreen; otherwise the
// port goes to charge only and a notification offers the other choices (a
// locked phone dozes, and every doze re-enumerated the undecided port on
// the computer). The same happens when the phone locks with the dialog
// open. While connected, a resident notification shows the mode (low
// urgency: no banner) and opens the dialog again.
//
// cezanne-usbd (org.cezanne.Usb, system bus, apps/cezanne-usb) owns the
// gadget and tells a computer from a charger; SetMode is allowed for the
// active session (polkit). In stream mode this starts the user unit
// cezanne-usb-stream.service, which feeds the webcam (screen through
// mutter's ScreenCast, cameras through libcamera).
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Dialog from 'resource:///org/gnome/shell/ui/dialog.js';
import * as MessageTray from 'resource:///org/gnome/shell/ui/messageTray.js';
import * as ModalDialog from 'resource:///org/gnome/shell/ui/modalDialog.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const BUS_NAME = 'org.cezanne.Usb';
const OBJ_PATH = '/org/cezanne/Usb';
const STREAM_UNIT = 'cezanne-usb-stream.service';
const ICON = 'drive-removable-media-symbolic';

const UsbIface = `<node><interface name="org.cezanne.Usb">
  <method name="SetMode"><arg name="mode" type="s" direction="in"/>
    <arg name="source" type="s" direction="in"/></method>
  <property name="Plugged" type="b" access="read"/>
  <property name="Host" type="b" access="read"/>
  <property name="Mode" type="s" access="read"/>
  <property name="Source" type="s" access="read"/>
  <property name="Sources" type="a(sss)" access="read"/>
  <property name="Error" type="s" access="read"/>
</interface></node>`;
const UsbProxy = Gio.DBusProxy.makeProxyWrapper(UsbIface);

// mode -> [title, description, icon]
const MODES = {
    stream: ['Video stream', 'The phone as a webcam: screen or a camera (OBS)',
        'camera-web-symbolic'],
    storage: ['Data storage', 'Files in your home folder (MTP)', 'folder-symbolic'],
    network: ['Developer network', 'USB network, the phone at 172.16.42.1',
        'network-wired-symbolic'],
    charge: ['Charge only', 'No data connection', 'battery-full-charging-symbolic'],
};

function row(title, description, iconName, onClick) {
    // expanded: the button centers a child that is not
    const box = new St.BoxLayout({style: 'spacing: 16px;', x_expand: true});
    box.add_child(new St.Icon({
        icon_name: iconName, icon_size: 24, y_align: Clutter.ActorAlign.CENTER,
    }));
    const text = new St.BoxLayout({
        orientation: Clutter.Orientation.VERTICAL, y_align: Clutter.ActorAlign.CENTER,
    });
    text.add_child(new St.Label({text: title, style: 'font-weight: bold;'}));
    text.add_child(new St.Label({text: description, style: 'font-size: smaller;'}));
    box.add_child(text);
    const button = new St.Button({
        style_class: 'button', child: box, x_expand: true, can_focus: true,
        style: 'padding: 12px 16px; text-align: left;',
    });
    button.connect('clicked', onClick);
    return button;
}

const UsbDialog = class {
    constructor(sources, choose) {
        this._sources = sources;
        this._choose = choose;
        this._dialog = new ModalDialog.ModalDialog({destroyOnClose: true});
        this._dialog.connect('destroy', () => (this._dialog = null));
        this._content = new Dialog.MessageDialogContent();
        this._list = new St.BoxLayout({
            orientation: Clutter.Orientation.VERTICAL, style: 'spacing: 8px;',
        });
        this._dialog.contentLayout.add_child(this._content);
        this._dialog.contentLayout.add_child(this._list);
    }

    open(page = 'modes') {
        if (page === 'sources')
            this._showSources();
        else
            this._showModes();
        this._dialog.open();
    }

    close() {
        this._dialog?.close();
    }

    get isOpen() {
        return this._dialog !== null;
    }

    _pick(mode, source = '') {
        this.close();
        this._choose(mode, source);
    }

    _showModes() {
        this._content.title = 'USB connected';
        this._content.description = 'Use USB for';
        this._list.destroy_all_children();
        for (const mode of ['stream', 'storage', 'network']) {
            const [title, description, icon] = MODES[mode];
            this._list.add_child(row(title, description, icon, () =>
                mode === 'stream' ? this._showSources() : this._pick(mode)));
        }
        this._dialog.setButtons([{
            label: MODES.charge[0], action: () => this._pick('charge'),
            key: Clutter.KEY_Escape, default: true,
        }]);
    }

    _showSources() {
        this._content.title = 'Video stream';
        this._content.description = 'The computer sees the phone as a webcam';
        this._list.destroy_all_children();
        for (const [id, label, camera] of this._sources) {
            this._list.add_child(row(label,
                camera ? 'Landscape, 1920×1080 or 1280×720' : 'Portrait, 1080×2400 or 720×1600',
                camera ? 'camera-photo-symbolic' : 'video-display-symbolic',
                () => this._pick('stream', id)));
        }
        this._dialog.setButtons([
            {label: 'Back', action: () => this._showModes()},
            {
                label: MODES.charge[0], action: () => this._pick('charge'),
                key: Clutter.KEY_Escape, default: true,
            },
        ]);
    }
};

export default class CezanneUsb extends Extension {
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
        this._host = false;
        this._mode = null;
        this._streamSource = '';
        this._announce = false;
        this._proxy = new UsbProxy(Gio.DBus.system, BUS_NAME, OBJ_PATH, (proxy, error) => {
            if (error) {
                console.log(`cezanne-usb: ${error.message}`);
                return;
            }
            proxy.connectObject(
                'g-properties-changed', () => this._sync(),
                'notify::g-name-owner', () => this._sync(), this);
            this._sync();
        });
        Main.sessionMode.connectObject('updated', () => this._sync(), this);
    }

    _disable() {
        this._proxy?.disconnectObject(this);
        this._proxy = null;
        Main.sessionMode.disconnectObject(this);
        this._dialog?.close();
        this._dialog = null;
        this._source?.destroy();
        this._source = null;
    }

    _sync() {
        const p = this._proxy;
        const host = Boolean(p?.g_name_owner && p.Plugged && p.Host);
        const mode = host ? p.Mode : null;
        const source = host ? p.Source : '';

        if (host && !this._host && mode === 'none')
            this._ask();
        if (host && this._dialog?.isOpen && Main.sessionMode.isLocked) {
            this._dialog.close();
            this._chargeOnly();
        }
        if (!host || (mode !== this._mode && mode !== 'none'))
            this._dialog?.close();	// unplugged, or decided elsewhere
        if (!host)
            this._notification?.destroy();
        if (mode !== this._mode || source !== this._streamSource)
            this._syncStream(mode, source);
        if (host && mode !== 'none' && (mode !== this._mode || source !== this._streamSource))
            this._notifyMode(mode, source);
        this._host = host;
        this._mode = mode;
        this._streamSource = source;
    }

    _ask() {
        const locked = Main.sessionMode.isLocked;
        const fullscreen = Main.layoutManager.primaryMonitor?.inFullscreen;
        if (locked || fullscreen)
            this._chargeOnly();
        else
            this._openDialog();
    }

    // not now: charge only, announced with a banner (_notifyMode)
    _chargeOnly() {
        this._announce = true;
        this._setMode('charge', '');
    }

    _openDialog(page) {
        if (this._dialog?.isOpen || !this._proxy || Main.sessionMode.isLocked)
            return;
        this._notification?.destroy();
        const sources = this._proxy.Sources ?? [];
        this._dialog = new UsbDialog(sources, (mode, source) => this._setMode(mode, source));
        this._dialog.open(page);
    }

    async _setMode(mode, source) {
        try {
            await this._proxy.SetModeAsync(mode, source);
        } catch (e) {
            if (e instanceof GLib.Error)
                Gio.DBusError.strip_remote_error(e);
            this._notify('USB mode failed', e.message, {transient: true});
        }
    }

    _syncStream(mode, source) {
        const method = mode === 'stream'
            ? this._mode === 'stream' ? 'RestartUnit' : 'StartUnit'
            : this._mode === 'stream' ? 'StopUnit' : null;
        if (!method)
            return;
        Gio.DBus.session.call('org.freedesktop.systemd1', '/org/freedesktop/systemd1',
            'org.freedesktop.systemd1.Manager', method,
            new GLib.Variant('(ss)', [STREAM_UNIT, 'replace']), null,
            Gio.DBusCallFlags.NONE, -1, null, (conn, res) => {
                try {
                    conn.call_finish(res);
                } catch (e) {
                    console.log(`cezanne-usb: ${method} ${STREAM_UNIT}: ${e.message}`);
                }
            });
    }

    _notifyMode(mode, source) {
        let [title] = MODES[mode];
        if (mode === 'stream') {
            const label = (this._proxy.Sources ?? []).find(s => s[0] === source)?.[1];
            title = `${title}: ${label ?? source}`;
        }
        const announce = this._announce && mode === 'charge';
        this._announce = false;
        this._notify(announce ? 'USB connected: charging only' : `USB: ${title}`,
            'Tap to choose what the USB port is for',
            {resident: true, urgent: announce, actions: mode === 'charge'});
    }

    _notify(title, body, {actions = false, resident = false, transient = false, urgent = false} = {}) {
        if (!this._source) {
            this._source = new MessageTray.Source({title: 'USB', iconName: ICON});
            this._source.connect('destroy', () => (this._source = null));
            Main.messageTray.add(this._source);
        }
        if (!transient)
            this._notification?.destroy();
        const notification = new MessageTray.Notification({
            source: this._source, title, body, iconName: ICON,
            resident, isTransient: transient,
            urgency: resident && !urgent ? MessageTray.Urgency.LOW : MessageTray.Urgency.NORMAL,
        });
        notification.connect('activated', () => this._openDialog());
        if (actions) {
            notification.addAction(MODES.stream[0], () => this._openDialog('sources'));
            notification.addAction(MODES.storage[0], () => this._setMode('storage', ''));
            notification.addAction(MODES.network[0], () => this._setMode('network', ''));
        }
        if (!transient) {
            this._notification = notification;
            notification.connect('destroy', () => {
                if (this._notification === notification)
                    this._notification = null;
            });
        }
        this._source.addNotification(notification);
    }
}
