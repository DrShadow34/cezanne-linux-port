// Styles the shell's menus ahead of their first open, GNOME 51.
//
// St computes an actor's theme node (CSS matching against every loaded rule,
// ~1400 with dash-to-dock's stylesheet) when the actor is first shown, and
// again for every new pseudo-class state (the panel button's :active while
// its menu is open); later opens reuse the nodes. After a shell start the
// first quick settings open took ~480 ms of main-thread CPU (45 % CSS
// matching) against ~180 ms for later ones: ~250 ms to its first frame and
// 4-7 frames in its 250 ms animation.
// A few seconds after the startup animation (the screen is still then) this
// walks the quick settings and calendar menus in idle slices of a few ms:
// ensure_style() on every widget, the panel buttons once with :active (put
// back within the same slice, so no frame shows it), preferred sizes
// bottom-up (text layouts, icons loaded at their CSS size); last, as one
// longer step, an invisible open and close of quick settings for what only
// opening styles. Again after a stylesheet change (light / dark), which
// drops every theme node. NOTES "GNOME startup stalls".
//
// Shell internals: Main.panel.statusArea.{quickSettings,dateMenu}.menu.actor
// (in Main.uiGroup while hidden), PanelMenu.Button's 'active' pseudo-class,
// PopupMenu open() / close() with {animate: false}.
import GLib from 'gi://GLib';
import St from 'gi://St';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

// after startup-complete: the deferred extension enables run first
const DELAY_MS = 3000;
// main-thread budget per idle slice
const SLICE_US = 4000;
const BUTTONS = ['quickSettings', 'dateMenu'];

// the walks span idle slices: an actor may be gone by the time it is reached
function children(actor) {
    try {
        return actor.get_children();
    } catch {
        return [];
    }
}

function* preorder(actor) {
    if (actor instanceof St.Widget)
        yield actor;
    for (const child of children(actor))
        yield* preorder(child);
}

function* postorder(actor) {
    for (const child of children(actor))
        yield* postorder(child);
    yield actor;
}

function* steps() {
    for (const name of BUTTONS) {
        const button = Main.panel.statusArea[name];
        if (!button)
            continue;
        yield () => {
            if (button.has_style_pseudo_class('active'))
                return;		// its menu is open: styled already
            button.add_style_pseudo_class('active');
            for (const w of preorder(button))
                w.ensure_style();
            button.remove_style_pseudo_class('active');
            for (const w of preorder(button))
                w.ensure_style();
        };
        const menu = button.menu?.actor;
        if (!menu)
            continue;
        for (const w of preorder(menu))
            yield () => w.ensure_style();
        // children first: each step lays out one actor from cached child sizes
        for (const a of postorder(menu))
            yield () => a.get_preferred_size();
    }
    // Opening restyles more than the walk reaches (open-state pseudo-classes,
    // items shown or built on open): quick settings opened and closed once,
    // invisible and without animation, in one step (no frame in between).
    // Not the calendar: opening it marks the notifications seen.
    yield () => {
        const menu = Main.panel.statusArea.quickSettings?.menu;
        if (!menu || menu.isOpen)
            return;
        const {actor} = menu;
        const {opacity} = actor;
        actor.opacity = 0;
        try {
            menu.open({animate: false});
            menu.close({animate: false});
        } finally {
            actor.opacity = opacity;
        }
    };
}

export default class CezanneWarmup extends Extension {
    enable() {
        const theme = St.ThemeContext.get_for_stage(global.stage);
        theme.connectObject('changed', () => this._schedule(), this);
        if (Main.layoutManager._startingUp) {
            Main.layoutManager.connectObject('startup-complete',
                () => this._schedule(), this);
        } else {
            this._schedule();
        }
    }

    _schedule() {
        this._stop();
        this._delayId = GLib.timeout_add(GLib.PRIORITY_LOW, DELAY_MS, () => {
            this._delayId = 0;
            this._run();
            return GLib.SOURCE_REMOVE;
        });
    }

    _run() {
        const it = steps();
        this._idleId = GLib.idle_add(GLib.PRIORITY_LOW, () => {
            const end = GLib.get_monotonic_time() + SLICE_US;
            do {
                try {
                    const {value: step, done} = it.next();
                    if (done) {
                        this._idleId = 0;
                        return GLib.SOURCE_REMOVE;
                    }
                    step();
                } catch {
                    // an actor destroyed since the walk began: skip it
                }
            } while (GLib.get_monotonic_time() < end);
            return GLib.SOURCE_CONTINUE;
        });
    }

    _stop() {
        if (this._delayId)
            GLib.source_remove(this._delayId);
        if (this._idleId)
            GLib.source_remove(this._idleId);
        this._delayId = this._idleId = 0;
    }

    disable() {
        this._stop();
        Main.layoutManager.disconnectObject(this);
        St.ThemeContext.get_for_stage(global.stage).disconnectObject(this);
    }
}
