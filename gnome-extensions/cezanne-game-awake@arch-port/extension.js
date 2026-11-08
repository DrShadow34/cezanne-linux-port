// Keep the phone awake while a game runs, GNOME 51.
//
// The phone sleeps once the session goes idle: after idle-delay (60 s)
// gsd-power blanks the screen, the screen locks, and cezanne-aod suspends.
// A game gives mutter no activity while it loads under Proton / FEX (a
// minute or more without a touch) or while it is played with a gamepad
// (read straight from evdev), so the phone locked and slept in the middle
// of it. Steam inhibits the screensaver through org.freedesktop.ScreenSaver,
// which GNOME does not serve.
//
// While a game runs this extension holds a gnome-session idle inhibitor
// (no blanking, no idle lock):
// - a Steam game: Steam starts each game under `reaper SteamLaunch AppId=N`,
//   looked for with pgrep every 10 s (a subprocess: no /proc walk on the
//   shell's thread), so the phone stays awake from the launch on;
// - a fullscreen window (games started outside Steam, videos).
// Locked (power key during a game) it lets go, so the phone still sleeps;
// it stays enabled on the lock screen like the project's other extensions
// (cezanne-lock-keep) and takes the inhibitor again after the unlock.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const SESSION_MANAGER = 'org.gnome.SessionManager';
const SESSION_MANAGER_PATH = '/org/gnome/SessionManager';
const INHIBIT_IDLE = 8;
const POLL_S = 10;
const STEAM_GAME = 'reaper SteamLaunch AppId=';

export default class GameAwakeExtension extends Extension {
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
        this._cookie = null;	// 0: Inhibit pending
        this._steamGame = false;
        this._pgrep = null;
        this._fullscreenId = global.display.connect('in-fullscreen-changed',
            () => this._update());
        this._modeId = Main.sessionMode.connect('updated', () => this._update());
        this._pollId = GLib.timeout_add_seconds(GLib.PRIORITY_LOW, POLL_S, () => {
            this._checkSteam();
            return GLib.SOURCE_CONTINUE;
        });
        this._checkSteam();
        this._update();
    }

    _disable() {
        global.display.disconnect(this._fullscreenId);
        Main.sessionMode.disconnect(this._modeId);
        GLib.source_remove(this._pollId);
        this._pgrep?.force_exit();
        this._pgrep = null;
        this._release();
    }

    _fullscreen() {
        for (let i = 0; i < global.display.get_n_monitors(); i++) {
            if (global.display.get_monitor_in_fullscreen(i))
                return true;
        }
        return false;
    }

    _checkSteam() {
        if (this._pgrep)
            return;
        let proc;
        try {
            proc = Gio.Subprocess.new(['pgrep', '-f', STEAM_GAME],
                Gio.SubprocessFlags.STDOUT_SILENCE | Gio.SubprocessFlags.STDERR_SILENCE);
        } catch (e) {
            console.log(`cezanne-game-awake: pgrep: ${e.message}`);
            return;
        }
        this._pgrep = proc;
        proc.wait_async(null, (p, res) => {
            try {
                p.wait_finish(res);
            } catch {}
            if (this._pgrep !== p)
                return;		// disabled meanwhile
            this._pgrep = null;
            this._steamGame = p.get_if_exited() && p.get_exit_status() === 0;
            this._update();
        });
    }

    _update() {
        const want = !Main.sessionMode.isLocked && (this._steamGame || this._fullscreen());
        if (want && this._cookie === null)
            this._inhibit();
        else if (!want && this._cookie !== null)
            this._release();
    }

    _inhibit() {
        this._cookie = 0;
        Gio.DBus.session.call(SESSION_MANAGER, SESSION_MANAGER_PATH,
            SESSION_MANAGER, 'Inhibit', new GLib.Variant('(susu)',
                ['org.gnome.Shell', 0, 'A game is running', INHIBIT_IDLE]),
            new GLib.VariantType('(u)'), Gio.DBusCallFlags.NONE, -1, null,
            (conn, res) => {
                let cookie = 0;
                try {
                    [cookie] = conn.call_finish(res).deepUnpack();
                } catch (e) {
                    console.log(`cezanne-game-awake: inhibit: ${e.message}`);
                }
                if (this._cookie === 0 && cookie)
                    this._cookie = cookie;
                else if (cookie)
                    this._uninhibit(cookie);	// released meanwhile
                else if (this._cookie === 0)
                    this._cookie = null;	// failed: try again on the next change
            });
    }

    _release() {
        const cookie = this._cookie;
        this._cookie = null;
        if (cookie)
            this._uninhibit(cookie);
    }

    _uninhibit(cookie) {
        Gio.DBus.session.call(SESSION_MANAGER, SESSION_MANAGER_PATH,
            SESSION_MANAGER, 'Uninhibit', new GLib.Variant('(u)', [cookie]),
            null, Gio.DBusCallFlags.NONE, -1, null, (conn, res) => {
                try {
                    conn.call_finish(res);
                } catch (e) {
                    console.log(`cezanne-game-awake: uninhibit: ${e.message}`);
                }
            });
    }
}
