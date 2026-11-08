// Smooth unlocking with third-party extensions, GNOME 51.
//
// On lock the shell disables every extension without the 'unlock-dialog'
// session mode and enables it again on unlock (ExtensionManager
// _sessionUpdated). Both directions cost:
// - its stylesheet is unloaded and loaded again. Any stylesheet change
//   makes St drop all theme nodes (St.ThemeContext 'changed') and match the
//   whole theme again for every actor: mapped ones at once (the unlock
//   animation stalled, ~400 ms to its first frame, gaps of ~150 ms), hidden
//   ones when next shown (the first quick settings open after an unlock:
//   ~240 ms of CSS matching, 4 frames instead of 23);
// - its actors are destroyed and built again, and new actors are styled
//   from scratch when first shown.
// The project's own extensions list 'unlock-dialog' and stay enabled. For
// third-party ones this extension
// - keeps the extensions in KEEP running through the lock (lock-safe:
//   quick settings items only, no app launches, nothing over the lock
//   screen);
// - keeps the stylesheet of one disabled for the lock screen loaded until
//   it is enabled again (dash-to-dock: its dock would lie above the lock
//   screen, and it rebuilds the dock on every session mode change anyway);
// - enables those only once the lock screen has slid away: the shell
//   enables them as the slide starts (ScreenShield._continueDeactivate pops
//   the session mode), and building dash-to-dock's dock held the slide's
//   first frame back by ~130 ms.
// At startup the third-party ones are not held back like the project's own
// extensions: dash-to-dock enabled after the startup animation re-zoomed the
// overview around its dock at the end of it (NOTES "GNOME startup stalls").
// Load order matters: an extension the shell disables on
// lock also disables and re-enables every extension enabled after it
// ("rebase"), so dash-to-dock goes last in org.gnome.shell
// enabled-extensions; the shell moves it there itself after the first
// unlock.
//
// Shell internals: ExtensionManager._extensionSupportsSessionMode,
// _loadExtensionStylesheet, _unloadExtensionStylesheet, extension.enabled
// (ui/extensionSystem.js): recheck after a GNOME upgrade. Measured with
// mainline/display/tools/wakeanim.sh.
import GLib from 'gi://GLib';
import St from 'gi://St';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {ExtensionState} from 'resource:///org/gnome/shell/misc/extensionUtils.js';
import {Extension, InjectionManager} from 'resource:///org/gnome/shell/extensions/extension.js';

// third-party extensions that stay enabled on the lock screen
const KEEP = ['auto-brightness-toggle@sao.studio'];
// the slide takes 250 ms (Overview.ANIMATION_TIME); enable anyway after this
const UNLOCK_WAIT_MS = 1000;

function theme() {
    return St.ThemeContext.get_for_stage(global.stage).get_theme();
}

export default class CezanneLockKeep extends Extension {
    enable() {
        const manager = Main.extensionManager;
        const proto = Object.getPrototypeOf(manager);
        // uuid -> {file, variant}: stylesheet left loaded over the lock
        const kept = this._kept = new Map();
        this._injections = new InjectionManager();
        this._injections.overrideMethod(proto, '_extensionSupportsSessionMode',
            supports => function (uuid) {
                return supports.call(this, uuid) ||
                    (KEEP.includes(uuid) && Main.sessionMode.currentMode === 'unlock-dialog');
            });
        this._injections.overrideMethod(proto, '_unloadExtensionStylesheet',
            unload => function (extension) {
                // disabled for the lock screen, not by the user
                if (extension.stylesheet && extension.enabled && Main.sessionMode.isLocked) {
                    kept.set(extension.uuid,
                        {file: extension.stylesheet, variant: Main.getStyleVariant()});
                    delete extension.stylesheet;
                    return;
                }
                unload.call(this, extension);
            });
        this._injections.overrideMethod(proto, '_loadExtensionStylesheet',
            load => function (extension) {
                const k = kept.get(extension.uuid);
                if (k) {
                    kept.delete(extension.uuid);
                    if (k.variant === Main.getStyleVariant()) {
                        extension.stylesheet = k.file;
                        return;
                    }
                    theme().unload_stylesheet(k.file);	// light / dark changed
                }
                load.call(this, extension);
            });
        const unlocked = () => this._unlocked();
        this._injections.overrideMethod(proto, '_callExtensionEnable',
            enable => async function (uuid) {
                // session unlocked, the lock screen still sliding away
                if (Main.screenShield?.locked && !Main.sessionMode.isLocked &&
                    this.lookup(uuid)?.state !== ExtensionState.ACTIVE)
                    await unlocked();
                return enable.call(this, uuid);
            });
        // turned off by the user while locked (gsettings): its stylesheet goes
        manager.connectObject('extension-state-changed', (_m, extension) => {
            const k = kept.get(extension.uuid);
            if (k && !extension.enabled) {
                kept.delete(extension.uuid);
                theme().unload_stylesheet(k.file);
            }
        }, this);
    }

    // resolves once the lock screen is gone (or after UNLOCK_WAIT_MS)
    _unlocked() {
        this._unlocking ??= new Promise(resolve => {
            const done = () => {
                Main.screenShield.disconnectObject(this);
                if (this._unlockWaitId)
                    GLib.source_remove(this._unlockWaitId);
                this._unlockWaitId = 0;
                this._unlocking = null;
                resolve();
            };
            this._unlockDone = done;
            Main.screenShield.connectObject('locked-changed', () => {
                if (!Main.screenShield.locked)
                    done();
            }, this);
            this._unlockWaitId = GLib.timeout_add(GLib.PRIORITY_DEFAULT,
                UNLOCK_WAIT_MS, () => {
                    this._unlockWaitId = 0;
                    done();
                    return GLib.SOURCE_REMOVE;
                });
        });
        return this._unlocking;
    }

    disable() {
        if (this._unlocking)
            this._unlockDone();
        Main.extensionManager.disconnectObject(this);
        this._injections.clear();
        this._injections = null;
        // nobody would unload them now: the extensions load them again
        for (const {file} of this._kept.values())
            theme().unload_stylesheet(file);
        this._kept = null;
    }
}
