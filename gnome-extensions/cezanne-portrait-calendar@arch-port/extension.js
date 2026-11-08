// Portrait layout for the clock popup (calendar and notifications), GNOME 51.
//
// The date menu (DateMenuButton) lays the notification list and the calendar
// column out side by side in an St.BoxLayout named "calendarArea". Together
// they are wider than the phone's portrait screen (540 logical px), and the
// list ends up off screen. In portrait this extension stacks them instead:
// the calendar column on top and the notification list below it, both
// filling the popup's width. The list's desktop styling (fixed width used as
// a minimum, right margin, 1px separator border, scrollbar reserve) is
// removed so the notification cards line up exactly with the events bar
// above them; the calendar grid is stretched to the same width and the
// weekday headings get two letters. When the screen rotates to landscape
// (auto-rotation) the stock layout comes back.
import Clutter from 'gi://Clutter';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

// Inline styles for portrait (inline beats the theme).
const AREA_STYLE = 'spacing: 8px;';   // + the events bar's 4px margin = the 12px between cards
const COLUMN_STYLE = 'margin: 0;';
const LIST_STYLE = 'margin: 0; padding: 0; border: 0; box-shadow: none; min-width: 0;';
const VIEW_STYLE = 'margin: 0 4px;';   // cards inset like the events bar (margin: 4px)

export default class CezannePortraitCalendar extends Extension {
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
        const dateMenu = Main.panel.statusArea.dateMenu;
        this._calendar = dateMenu._calendar;
        this._list = dateMenu._messageList;
        this._column = this._calendar.get_parent();
        this._area = this._column.get_parent();
        this._portrait = false;

        // The calendar recreates its cells on every month or date change and
        // its headings when the week start changes: redo the portrait tweaks.
        const calendar = this._calendar;
        this._origRebuildCalendar = calendar._rebuildCalendar;
        this._origBuildHeader = calendar._buildHeader;
        calendar._rebuildCalendar = (...args) => {
            this._origRebuildCalendar.apply(calendar, args);
            if (this._portrait)
                this._fillCalendar(true);
        };
        calendar._buildHeader = (...args) => {
            this._origBuildHeader.apply(calendar, args);
            if (this._portrait)
                this._setHeadings(true);
        };

        this._monitorsChangedId = Main.layoutManager.connect('monitors-changed',
            () => this._update());
        this._update();
    }

    _disable() {
        Main.layoutManager.disconnect(this._monitorsChangedId);
        this._setPortrait(false);
        this._calendar._rebuildCalendar = this._origRebuildCalendar;
        this._calendar._buildHeader = this._origBuildHeader;
        this._calendar = null;
        this._list = null;
        this._column = null;
        this._area = null;
    }

    _update() {
        const monitor = Main.layoutManager.primaryMonitor;   // undefined without monitors
        this._setPortrait(monitor ? monitor.height > monitor.width : false);
    }

    _setPortrait(portrait) {
        if (portrait === this._portrait)
            return;
        this._portrait = portrait;

        const {_area: area, _column: column, _list: list} = this;
        const view = list._messageView;
        const scrollView = list._scrollView;
        if (portrait) {
            this._saved = {
                orientation: area.layout_manager.orientation,
                area: area.style,
                column: column.style,
                list: list.style,
                view: view.style,
                overlayScrollbars: scrollView.overlay_scrollbars,
            };
            area.layout_manager.orientation = Clutter.Orientation.VERTICAL;
            area.set_child_at_index(column, 0);
            area.style = AREA_STYLE;
            column.style = COLUMN_STYLE;
            list.style = LIST_STYLE;
            view.style = VIEW_STYLE;
            scrollView.overlay_scrollbars = true;   // no space reserved beside the cards
        } else {
            const saved = this._saved;
            area.layout_manager.orientation = saved.orientation;
            area.set_child_at_index(list, 0);
            area.style = saved.area;
            column.style = saved.column;
            list.style = saved.list;
            view.style = saved.view;
            scrollView.overlay_scrollbars = saved.overlayScrollbars;
        }
        this._fillCalendar(portrait);
        this._setHeadings(portrait);
    }

    // The grid only widens its columns when the cells expand.
    _fillCalendar(fill) {
        for (const child of this._calendar.get_children())
            child.x_expand = fill;
    }

    // Two-letter weekday headings (Mo Tu We ...) instead of one. Each heading
    // label carries the full weekday name as its accessible name.
    _setHeadings(twoLetters) {
        for (const child of this._calendar.get_children()) {
            if (!child.has_style_class_name('calendar-day-heading'))
                continue;
            if (twoLetters) {
                child._cezanneText ??= child.text;
                child.text = child.accessible_name.substring(0, 2);
            } else if (child._cezanneText !== undefined) {
                child.text = child._cezanneText;
                delete child._cezanneText;
            }
        }
    }
}
