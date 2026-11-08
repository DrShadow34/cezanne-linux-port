// SPDX-License-Identifier: GPL-2.0-or-later
// Quick Settings flashlight: the rear flash LEDs (/sys/class/leds/*:flash-*)
// in torch mode. Writable for the video group (udev 64-cezanne-flash.rules).

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {QuickMenuToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';

const LEDS_DIR = '/sys/class/leds';
const LEVELS = [
    ['Low', 0.25],
    ['Medium', 0.5],
    ['High', 1.0],
];

function flashLeds() {
    const leds = [];
    const dir = Gio.File.new_for_path(LEDS_DIR);
    let iter;
    try {
        iter = dir.enumerate_children('standard::name', Gio.FileQueryInfoFlags.NONE, null);
    } catch {
        return leds;
    }
    let info;
    while ((info = iter.next_file(null)) !== null) {
        const name = info.get_name();
        if (name.includes(':flash'))
            leds.push(`${LEDS_DIR}/${name}`);
    }
    return leds;
}

function readInt(path) {
    try {
        const [, bytes] = GLib.file_get_contents(path);
        return parseInt(new TextDecoder().decode(bytes), 10) || 0;
    } catch {
        return 0;
    }
}

// sysfs attributes must be written in place (no temporary file + rename)
function writeValue(path, value) {
    try {
        const stream = Gio.File.new_for_path(path).append_to(Gio.FileCreateFlags.NONE, null);
        stream.write_all(new TextEncoder().encode(`${value}\n`), null);
        stream.close(null);
    } catch (e) {
        console.warn(`Flashlight: ${path}: ${e.message}`);
    }
}

const FlashlightToggle = GObject.registerClass(
class FlashlightToggle extends QuickMenuToggle {
    constructor(extension) {
        super({
            title: 'Flashlight',
            gicon: Gio.icon_new_for_string(`${extension.path}/icons/flashlight-symbolic.svg`),
            toggleMode: true,
        });

        this._leds = flashLeds();
        this._level = LEVELS[1][1];

        this.menu.setHeader(this.gicon, 'Flashlight');
        this._items = LEVELS.map(([label, level]) => {
            const item = new PopupMenu.PopupMenuItem(label);
            item.connect('activate', () => {
                this._level = level;
                this._updateItems();
                if (this.checked)
                    this._apply();
                else
                    this.checked = true;
            });
            this.menu.addMenuItem(item);
            return [item, level];
        });
        this._updateItems();

        // reflect the LEDs as they are (e.g. lit by the camera)
        this.checked = this._leds.some(led => readInt(`${led}/brightness`) > 0);
        this.connect('notify::checked', () => this._apply());
    }

    _updateItems() {
        for (const [item, level] of this._items)
            item.setOrnament(level === this._level ? PopupMenu.Ornament.CHECK : PopupMenu.Ornament.NONE);
    }

    _apply() {
        for (const led of this._leds) {
            const max = readInt(`${led}/max_brightness`) || 1;
            const value = this.checked ? Math.max(1, Math.round(this._level * max)) : 0;
            writeValue(`${led}/brightness`, value);
        }
    }

    switchOff() {
        if (this.checked)
            this.checked = false;
    }
});

const FlashlightIndicator = GObject.registerClass(
class FlashlightIndicator extends SystemIndicator {
    constructor(extension) {
        super();

        this._indicator = this._addIndicator();
        this._indicator.gicon = Gio.icon_new_for_string(`${extension.path}/icons/flashlight-symbolic.svg`);

        this._toggle = new FlashlightToggle(extension);
        this._toggle.bind_property('checked', this._indicator, 'visible',
            GObject.BindingFlags.SYNC_CREATE);
        this.quickSettingsItems.push(this._toggle);
    }

    destroy() {
        this._toggle.switchOff();
        this.quickSettingsItems.forEach(item => item.destroy());
        super.destroy();
    }
});

export default class FlashlightExtension extends Extension {
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
        if (flashLeds().length === 0)
            return;
        this._indicator = new FlashlightIndicator(this);
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    _disable() {
        this._indicator?.destroy();
        this._indicator = null;
    }
}
