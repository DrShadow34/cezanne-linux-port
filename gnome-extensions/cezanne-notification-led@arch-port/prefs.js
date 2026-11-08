// SPDX-License-Identifier: GPL-2.0-or-later
// Preferences: default and per-app notification colours, battery light.

import Adw from 'gi://Adw';
import Gdk from 'gi://Gdk';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gtk from 'gi://Gtk';

import {ExtensionPreferences} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

function toHex(rgba) {
    const c = v => Math.round(v * 255).toString(16).padStart(2, '0');
    return `#${c(rgba.red)}${c(rgba.green)}${c(rgba.blue)}`;
}

function toRgba(hex) {
    const rgba = new Gdk.RGBA();
    rgba.parse(hex);
    return rgba;
}

function colorButton(hex) {
    const button = new Gtk.ColorDialogButton({
        dialog: new Gtk.ColorDialog({with_alpha: false}),
        rgba: toRgba(hex),
        valign: Gtk.Align.CENTER,
    });
    return button;
}

export default class NotificationLightPreferences extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        const settings = this.getSettings();
        window._settings = settings;
        window.set_search_enabled(true);

        const page = new Adw.PreferencesPage();
        window.add(page);

        const general = new Adw.PreferencesGroup({title: 'Notification Light'});
        page.add(general);

        const defaultRow = new Adw.ActionRow({
            title: 'Notification colour',
            subtitle: 'For apps without their own colour',
        });
        const defaultButton = colorButton(settings.get_string('notification-color'));
        defaultButton.connect('notify::rgba', () =>
            settings.set_string('notification-color', toHex(defaultButton.rgba)));
        defaultRow.add_suffix(defaultButton);
        general.add(defaultRow);

        const vibrateRow = new Adw.SwitchRow({
            title: 'Vibrate',
            subtitle: 'On new notifications',
        });
        settings.bind('vibrate', vibrateRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        general.add(vibrateRow);

        const batteryRow = new Adw.SwitchRow({
            title: 'Battery light',
            subtitle: 'Red while charging, green when charged',
        });
        settings.bind('battery-light', batteryRow, 'active', Gio.SettingsBindFlags.DEFAULT);
        general.add(batteryRow);

        const chargedRow = new Adw.SpinRow({
            title: 'Charged at',
            subtitle: 'Battery percentage shown green',
            adjustment: new Gtk.Adjustment({lower: 50, upper: 100, step_increment: 1}),
        });
        settings.bind('charged-percentage', chargedRow, 'value', Gio.SettingsBindFlags.DEFAULT);
        settings.bind('battery-light', chargedRow, 'sensitive', Gio.SettingsBindFlags.GET);
        general.add(chargedRow);

        const apps = new Adw.PreferencesGroup({
            title: 'App Colours',
            description: 'Change an app’s colour to give it its own; reset to use the default.',
        });
        page.add(apps);

        const infos = Gio.AppInfo.get_all()
            .filter(a => a.should_show() && a.get_id())
            .sort((a, b) => a.get_display_name().localeCompare(b.get_display_name()));
        for (const info of infos)
            apps.add(this._appRow(settings, info, defaultButton));
    }

    _appRow(settings, info, defaultButton) {
        const appId = info.get_id().replace(/\.desktop$/, '');
        const row = new Adw.ActionRow({title: info.get_display_name()});
        const icon = info.get_icon();
        if (icon)
            row.add_prefix(new Gtk.Image({gicon: icon, pixel_size: 32}));

        const colors = () => settings.get_value('app-colors').deepUnpack();
        const custom = colors()[appId];
        const button = colorButton(custom ?? settings.get_string('notification-color'));
        const reset = new Gtk.Button({
            icon_name: 'edit-undo-symbolic',
            tooltip_text: 'Use the default colour',
            valign: Gtk.Align.CENTER,
            css_classes: ['flat'],
            visible: custom !== undefined,
        });

        let updating = false;
        button.connect('notify::rgba', () => {
            if (updating)
                return;
            const map = colors();
            map[appId] = toHex(button.rgba);
            settings.set_value('app-colors', new GLib.Variant('a{ss}', map));
            reset.visible = true;
            row.subtitle = 'Own colour';
        });
        reset.connect('clicked', () => {
            const map = colors();
            delete map[appId];
            settings.set_value('app-colors', new GLib.Variant('a{ss}', map));
            reset.visible = false;
            row.subtitle = '';
            updating = true;
            button.rgba = defaultButton.rgba;
            updating = false;
        });
        // follow the default colour while the app has none
        defaultButton.connect('notify::rgba', () => {
            if (colors()[appId] !== undefined)
                return;
            updating = true;
            button.rgba = defaultButton.rgba;
            updating = false;
        });
        if (custom !== undefined)
            row.subtitle = 'Own colour';

        row.add_suffix(reset);
        row.add_suffix(button);
        return row;
    }
}
