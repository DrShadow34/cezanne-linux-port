// Quick settings for the cezanne panel:
// - DC dimming toggle: the panel driver keeps the panel out of its low PWM
//   range and dims the pixels in the display pipeline instead (sysfs
//   dc_dimming).
// - Colour scheme menu, MIUI's: the panel's colour correction tables (sysfs
//   color_mode).
// The choices are kept in ~/.config/cezanne/ and applied when the session
// starts.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import * as QuickSettings from 'resource:///org/gnome/shell/ui/quickSettings.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const PANEL = '/sys/bus/mipi-dsi/devices/1400e000.dsi.0';
const CONFIG = GLib.build_filenamev([GLib.get_user_config_dir(), 'cezanne']);

// MIUI names: Vivid is the panel's init default
const COLOR_MODES = [
    ['p3', 'Vivid'],
    ['p3-d65', 'Natural'],
    ['srgb', 'Standard'],
    ['native', 'Saturated'],
];

function readStr(path, fallback) {
    try {
        const [, b] = GLib.file_get_contents(path);
        return new TextDecoder().decode(b).trim();
    } catch {
        return fallback;
    }
}

// The panel attributes run DCS sequences (dc_dimming re-applies the level
// with a frame: ~60 ms that blocked the shell when it was written in place):
// written from a GIO worker thread, in order.
let writes = Promise.resolve();
function writeStr(path, s) {
    GLib.mkdir_with_parents(GLib.path_get_dirname(path), 0o755);
    writes = writes.then(() => new Promise(resolve => {
        Gio.File.new_for_path(path).replace_contents_bytes_async(
            new GLib.Bytes(new TextEncoder().encode(s)), null, false,
            Gio.FileCreateFlags.NONE, null, (file, res) => {
                try {
                    file.replace_contents_finish(res);
                } catch (e) {
                    console.log(`cezanne-display: ${path}: ${e.message}`);
                }
                resolve();
            });
    }));
}

const DcToggle = GObject.registerClass(
class DcToggle extends QuickSettings.QuickToggle {
    constructor() {
        super({
            title: 'DC Dimming',
            iconName: 'display-brightness-symbolic',
            toggleMode: true,
        });
        const state = `${CONFIG}/dc-dimming`;
        this.checked = readStr(state, '0') === '1';
        writeStr(`${PANEL}/dc_dimming`, this.checked ? '1' : '0');
        this.connect('notify::checked', () => {
            const v = this.checked ? '1' : '0';
            writeStr(`${PANEL}/dc_dimming`, v);
            writeStr(state, `${v}\n`);
        });
    }
});

const ColorToggle = GObject.registerClass(
class ColorToggle extends QuickSettings.QuickMenuToggle {
    constructor() {
        super({
            title: 'Colors',
            iconName: 'preferences-color-symbolic',
            toggleMode: false,
        });
        this._state = `${CONFIG}/color-mode`;
        this.menu.setHeader('preferences-color-symbolic', 'Color scheme');
        this._items = new Map();
        for (const [mode, name] of COLOR_MODES) {
            const item = new PopupMenu.PopupMenuItem(name);
            item.connect('activate', () => this._set(mode));
            this.menu.addMenuItem(item);
            this._items.set(mode, item);
        }
        // the tile itself steps through the schemes
        this.connect('clicked', () => {
            const i = COLOR_MODES.findIndex(([m]) => m === this._mode);
            this._set(COLOR_MODES[(i + 1) % COLOR_MODES.length][0]);
        });
        this._set(readStr(this._state, 'p3'));
    }

    _set(mode) {
        if (!COLOR_MODES.some(([m]) => m === mode))
            mode = 'p3';
        this._mode = mode;
        writeStr(`${PANEL}/color_mode`, mode);
        writeStr(this._state, `${mode}\n`);
        this.subtitle = COLOR_MODES.find(([m]) => m === mode)[1];
        for (const [m, item] of this._items) {
            item.setOrnament(m === mode
                ? PopupMenu.Ornament.CHECK : PopupMenu.Ornament.NONE);
        }
    }
});

const DisplayIndicator = GObject.registerClass(
class DisplayIndicator extends QuickSettings.SystemIndicator {
    constructor() {
        super();
        this.quickSettingsItems.push(new DcToggle(), new ColorToggle());
    }

    destroy() {
        this.quickSettingsItems.forEach(i => i.destroy());
        super.destroy();
    }
});

export default class CezanneDisplay extends Extension {
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
        this._indicator = new DisplayIndicator();
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    _disable() {
        this._indicator.destroy();
        this._indicator = null;
    }
}
