// Phone behaviour for the on-screen keyboard, GNOME 51.
//
// The stock OSK (ui/keyboard.js, shown automatically in touch mode) keeps
// the focused text field visible by sliding the focused window up by the
// keyboard height: it eases the window *actor* up, then moves the window
// with move_frame(). A maximized window cannot move, so only its picture
// moves and touches land ~300 px away from what is shown; when the keyboard
// then closes through the "let go" path (_onFocusWindowMoving) the actor
// is never moved back and the window stays drawn too high (NOTES "GNOME:
// on-screen keyboard leaves maximized windows shifted").
//
// Here maximized and fullscreen windows are never slid. Instead, while the
// keyboard is up, an invisible chrome actor the size of the keyboard sits
// on the bottom edge as a strut, so the work area ends above the keyboard
// and maximized windows resize to fit (as Android's adjustResize / Phosh).
// Window actors that were left displaced are put back where their window
// is: on enable, and whenever the keyboard hides. Other windows keep the
// stock slide, which moves them for real.
//
//
// The keyboard also rebuilds every key (all levels, ~250 ms here) on each
// input purpose change, even hidden: on the lock screen that is every swipe
// (the password entry takes and drops the focus), the stall at the start of
// the swipe. NORMAL / ALPHA / PASSWORD give the same layout: skipped. And
// the keyboard is built at login instead of on the session's first touch.
//
// Shell internals touched: Keyboard.prototype._animateWindow, gestureProgress
// and _updateKeys (wrapped), Main.keyboard 'visibility-changed', _lastDevice
// and _syncEnabled(), Main.layoutManager.keyboardBox / keyboardMonitor,
// addChrome struts.
import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Keyboard} from 'resource:///org/gnome/shell/ui/keyboard.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

// An actor that is never picked, so it cannot become the target of a touch
// (some shell code picks with Clutter.PickMode.ALL, which also finds
// non-reactive actors).
const Strut = GObject.registerClass(
class CezanneOskStrut extends Clutter.Actor {
    vfunc_pick(_pickContext) {
    }
});

function pinned(window) {
    return window.maximized_vertically || window.is_fullscreen();
}

// Put a window's actor back on its window (mutter keeps the actor at the
// buffer rect; only the OSK slide moves it elsewhere).
function resync(window) {
    const actor = window.get_compositor_private();
    if (!actor)
        return;
    const rect = window.get_buffer_rect();
    if (actor.x === rect.x && actor.y === rect.y)
        return;
    actor.remove_transition('x');
    actor.remove_transition('y');
    actor.set_position(rect.x, rect.y);
}

// which layout Keyboard._updateLayout() builds for a purpose
function layoutKind(purpose) {
    const P = Clutter.InputContentPurpose;
    switch (purpose) {
    case P.DIGITS: return 'digits';
    case P.NUMBER: return 'number';
    case P.PHONE: return 'phone';
    case P.EMAIL: return 'email';
    case P.URL: return 'url';
    case P.TERMINAL: return 'terminal';
    default: return 'text';
    }
}

function resyncAll() {
    for (const actor of global.get_window_actors()) {
        const window = actor.get_meta_window();
        if (window && pinned(window))
            resync(window);
    }
}

export default class CezanneOsk extends Extension {
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
        const proto = Keyboard.prototype;
        const origAnimateWindow = proto._animateWindow;
        const origGestureProgress = proto.gestureProgress;
        const origUpdateKeys = proto._updateKeys;
        this._orig = {origAnimateWindow, origGestureProgress, origUpdateKeys};

        proto._updateKeys = function () {
            const layout = `${this._keyboardController.getCurrentGroup()} ` +
                layoutKind(this._keyboardController.purpose);
            if (this._currentLayout && layout === this._cezanneLayout) {
                this._setActiveLevel('default');
                return;
            }
            this._cezanneLayout = layout;
            origUpdateKeys.call(this);
        };

        proto._animateWindow = function (window, show) {
            if (pinned(window)) {
                resync(window);
                return;
            }
            origAnimateWindow.call(this, window, show);
        };
        // Swiping the keyboard up from the bottom edge moves the focus
        // window's actor along with it.
        proto.gestureProgress = function (delta) {
            origGestureProgress.call(this, delta);
            if (this._focusWindow && pinned(this._focusWindow))
                resync(this._focusWindow);
        };

        this._strut = new Strut({name: 'cezanneOskStrut', reactive: false});
        this._tracked = false;
        this._visibilityId = Main.keyboard.connect('visibility-changed', () => this._sync());
        this._heightId = Main.layoutManager.keyboardBox.connect('notify::height', () => this._sync());
        this._monitorsId = Main.layoutManager.connect('monitors-changed', () => this._sync());
        this._sync();
        resyncAll();

        // build the keyboard now, not on the first touch (a swipe's start)
        this._prebuildId = GLib.idle_add(GLib.PRIORITY_LOW, () => {
            this._prebuildId = 0;
            const keyboard = Main.keyboard;
            const seat = global.stage.context.get_backend().get_default_seat();
            const touchscreen = seat.list_devices().find(d =>
                d.get_device_type() === Clutter.InputDeviceType.TOUCHSCREEN_DEVICE);
            if (!keyboard._keyboard && !keyboard._lastDevice && touchscreen &&
                seat.get_touch_mode()) {
                keyboard._lastDevice = touchscreen;
                keyboard._syncEnabled();
            }
            return GLib.SOURCE_REMOVE;
        });
    }

    _disable() {
        if (this._prebuildId)
            GLib.source_remove(this._prebuildId);
        this._prebuildId = 0;
        Main.keyboard.disconnect(this._visibilityId);
        Main.layoutManager.keyboardBox.disconnect(this._heightId);
        Main.layoutManager.disconnect(this._monitorsId);
        this._setStrut(false);
        this._strut.destroy();
        this._strut = null;

        const proto = Keyboard.prototype;
        proto._animateWindow = this._orig.origAnimateWindow;
        proto.gestureProgress = this._orig.origGestureProgress;
        proto._updateKeys = this._orig.origUpdateKeys;
        this._orig = null;
    }

    // The strut follows the keyboard: there while it is visible, as tall as
    // the keyboard box, on the keyboard's monitor.
    _sync() {
        const box = Main.layoutManager.keyboardBox;
        const monitor = Main.layoutManager.keyboardMonitor;
        // Main.keyboard.visible is null while no OSK exists (not in touch mode)
        const visible = Boolean(Main.keyboard.visible && monitor && box.height > 0);
        if (visible) {
            const height = Math.min(Math.round(box.height), monitor.height);
            this._strut.set_position(monitor.x, monitor.y + monitor.height - height);
            this._strut.set_size(monitor.width, height);
        }
        this._setStrut(visible);
        if (!visible)
            resyncAll();
    }

    _setStrut(on) {
        if (on === this._tracked)
            return;
        // GNOME 51 chrome takes trackFullscreen / affectsStruts only (no
        // input region on Wayland; an unknown parameter throws)
        if (on)
            Main.layoutManager.addChrome(this._strut, {affectsStruts: true});
        else
            Main.layoutManager.removeChrome(this._strut);
        this._tracked = on;
    }
}
