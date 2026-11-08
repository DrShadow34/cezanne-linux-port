// Android-style "Show taps" for the touchscreen, GNOME 51.
//
// Every finger on the screen gets a translucent white spot that follows it
// and fades out when the finger lifts, over apps and the shell alike (handy
// for screen recordings and demos). Turn it on and off with the Extensions
// app, as Android's developer option.
//
// Touch events reach Clutter even when they go to a Wayland client (mutter's
// meta_wayland_touch_handle_event() lets them propagate), so a Clutter event
// filter sees every touch, before any shell grab. The filter only watches;
// it never consumes an event. The spots live in a layer on top of the stage
// that is never picked, so they cannot become the target of a touch. While
// a spot is shown, unredirection is off: a fullscreen app scanned out
// directly would otherwise hide the layer. A touch device that goes away
// with fingers down (a remote-desktop virtual touchscreen) sends no end
// for them: its spots go with its DEVICE_REMOVED event.
import Clutter from 'gi://Clutter';
import GObject from 'gi://GObject';
import St from 'gi://St';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const SPOT_SIZE = 44;		// diameter, logical px (5.5 mm on the panel)
const FADE_MS = 150;
const SPOT_STYLE = `background-color: rgba(255, 255, 255, 0.5);
    border: 2px solid rgba(0, 0, 0, 0.3); border-radius: ${SPOT_SIZE / 2}px;`;

const TOUCH_BEGIN = Clutter.EventType.TOUCH_BEGIN;
const TOUCH_UPDATE = Clutter.EventType.TOUCH_UPDATE;
const TOUCH_END = Clutter.EventType.TOUCH_END;
const TOUCH_CANCEL = Clutter.EventType.TOUCH_CANCEL;
const DEVICE_REMOVED = Clutter.EventType.DEVICE_REMOVED;

const Layer = GObject.registerClass(
class CezanneTapsLayer extends Clutter.Actor {
    vfunc_pick(_pickContext) {
    }
});

export default class CezanneShowTaps extends Extension {
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
        this._spots = new Map();	// input device -> (touch slot -> spot)
        this._shown = 0;		// spots on screen, fading ones included
        this._layer = new Layer({name: 'cezanneTapsLayer', reactive: false});
        this._layer.add_constraint(new Clutter.BindConstraint({
            source: global.stage, coordinate: Clutter.BindCoordinate.SIZE,
        }));
        global.stage.add_child(this._layer);
        // No stage: a filter bound to one looks up the stage of each event's
        // actor, and device events have none (CRITICAL, and they are skipped).
        this._filterId = Clutter.Event.add_filter(null,
            event => this._onEvent(event));
    }

    _disable() {
        Clutter.Event.remove_filter(this._filterId);
        const layer = this._layer;
        this._layer = null;	// fading spots stop counting
        layer.destroy();	// takes the spots with it
        this._spots = null;
        if (this._shown > 0)
            global.compositor.enable_unredirect();
        this._shown = 0;
    }

    _onEvent(event) {
        const type = event.type();
        if (type === DEVICE_REMOVED) {
            const spots = this._spots.get(event.get_source_device());
            for (const [slot, spot] of spots ?? [])
                this._release(spots, slot, spot);
            return Clutter.EVENT_PROPAGATE;
        }
        if (type !== TOUCH_BEGIN && type !== TOUCH_UPDATE &&
            type !== TOUCH_END && type !== TOUCH_CANCEL)
            return Clutter.EVENT_PROPAGATE;

        const device = event.get_source_device();
        let spots = this._spots.get(device);
        if (!spots) {
            spots = new Map();
            this._spots.set(device, spots);
        }
        const slot = event.get_event_sequence()?.get_slot() ?? 0;
        const [x, y] = event.get_coords();
        let spot = spots.get(slot);

        if (type === TOUCH_BEGIN) {
            if (spot)	// a lost end: start over
                this._release(spots, slot, spot);
            spot = this._addSpot();
            spots.set(slot, spot);
        }
        if (!spot)
            return Clutter.EVENT_PROPAGATE;

        spot.set_position(Math.round(x - SPOT_SIZE / 2), Math.round(y - SPOT_SIZE / 2));
        if (type === TOUCH_END || type === TOUCH_CANCEL)
            this._release(spots, slot, spot);
        return Clutter.EVENT_PROPAGATE;
    }

    _addSpot() {
        const spot = new St.Widget({style: SPOT_STYLE, width: SPOT_SIZE, height: SPOT_SIZE});
        this._layer.add_child(spot);
        // Stay above whatever was added to the stage since.
        global.stage.set_child_above_sibling(this._layer, null);
        if (this._shown++ === 0)
            global.compositor.disable_unredirect();
        return spot;
    }

    _release(spots, slot, spot) {
        spots.delete(slot);
        spot.ease({
            opacity: 0,
            duration: FADE_MS,
            mode: Clutter.AnimationMode.EASE_OUT_QUAD,
            onStopped: () => {
                if (!this._layer)	// disabled: destroyed with the layer
                    return;
                spot.destroy();
                if (--this._shown === 0)
                    global.compositor.enable_unredirect();
            },
        });
    }
}
