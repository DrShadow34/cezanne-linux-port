// Close buttons on the overview's window previews for touch, GNOME 51.
//
// The stock window picker shows a preview's title and close button only
// while the pointer is over it (or it has the key focus, or after a long
// press): a touchscreen has no hover, so windows could not be closed from
// the overview. Here the close button stays up on every preview whenever
// the stock code would allow its overlay (the active workspace, overview
// fully in the window picker: WorkspaceLayout._syncOverlay ->
// overlayEnabled), fading in once the overview has opened; it goes away
// while the preview is dragged. The title and the hover zoom keep the stock
// behaviour.
//
// Shell internals touched: WindowPreview.prototype showOverlay / hideOverlay
// (wrapped), _closeButton, _overlayShown, inDrag, _windowCanClose(),
// WorkspaceLayout.prototype._syncOverlay (wrapped), the previews'
// 'drag-end' signal.
import Clutter from 'gi://Clutter';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {WindowPreview} from 'resource:///org/gnome/shell/ui/windowPreview.js';
import {WorkspaceLayout} from 'resource:///org/gnome/shell/ui/workspace.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const FADE_MS = 200;	// WINDOW_OVERLAY_FADE_TIME

function steady(button) {
    return button.visible && button.opacity === 255 &&
        !button.get_transition('opacity');
}

export default class CezanneOverviewClose extends Extension {
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
        const previews = new Set();
        this._previews = previews;

        // Close button up whenever the overlay could be, hover or not.
        const sync = preview => {
            const button = preview._closeButton;
            if (preview.overlayEnabled && !preview.inDrag &&
                preview._windowCanClose()) {
                if (steady(button))
                    return;
                if (!button.visible) {
                    button.opacity = 0;
                    button.show();
                }
                button.remove_transition('opacity');
                button.ease({
                    opacity: 255,
                    duration: FADE_MS,
                    mode: Clutter.AnimationMode.EASE_OUT_QUAD,
                });
            } else if (!preview._overlayShown) {
                button.remove_transition('opacity');
                button.hide();
            }
        };
        const track = preview => {
            if (previews.has(preview))
                return;
            previews.add(preview);
            preview.connectObject(
                'drag-end', () => sync(preview),
                'destroy', () => previews.delete(preview),
                this);
        };

        const layoutProto = WorkspaceLayout.prototype;
        const proto = WindowPreview.prototype;
        const origSyncOverlay = layoutProto._syncOverlay;
        const origShowOverlay = proto.showOverlay;
        const origHideOverlay = proto.hideOverlay;
        this._orig = {origSyncOverlay, origShowOverlay, origHideOverlay};

        // called for every new preview and on every overview state change
        layoutProto._syncOverlay = function (preview) {
            origSyncOverlay.call(this, preview);
            track(preview);
            sync(preview);
        };
        // the stock fade-in would blink a close button that is already up
        proto.showOverlay = function (animate) {
            const wasUp = steady(this._closeButton);
            origShowOverlay.call(this, animate);
            if (wasUp) {
                this._closeButton.remove_transition('opacity');
                this._closeButton.opacity = 255;
            }
        };
        proto.hideOverlay = function (animate) {
            origHideOverlay.call(this, animate);
            sync(this);
        };
    }

    _disable() {
        const layoutProto = WorkspaceLayout.prototype;
        const proto = WindowPreview.prototype;
        layoutProto._syncOverlay = this._orig.origSyncOverlay;
        proto.showOverlay = this._orig.origShowOverlay;
        proto.hideOverlay = this._orig.origHideOverlay;
        this._orig = null;

        // back to the stock state: the close button only with the overlay
        for (const preview of this._previews) {
            preview.disconnectObject(this);
            if (!preview._overlayShown) {
                preview._closeButton.remove_transition('opacity');
                preview._closeButton.hide();
            }
        }
        this._previews = null;
    }
}
