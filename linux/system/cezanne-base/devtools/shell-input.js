#!/usr/bin/gjs -m
// shell-input: inject keyboard and pointer input into the running GNOME
// session through mutter's org.gnome.Mutter.RemoteDesktop D-Bus API, the
// same path screen-sharing clients use. Works on the phone, which has no
// keyboard and no uinput, so the shell UI (menus, quick settings, apps) can
// be driven from an ssh session for testing and screenshots (see `scr`).
//
// Run as the session user with its session bus (the shell-input wrapper
// does that from root):
//   shell-input key super+v          press and release a key combination
//   shell-input key Escape           keysym names: X11 names or single chars
//   shell-input move 100 -20         relative pointer motion (logical px)
//   shell-input moveto 200 300       pointer to a screen position (logical px)
//   shell-input click [1|2|3]        pointer button press and release, then park
//                                    the pointer in the bottom-right corner
//   shell-input scroll <dy>          wheel steps (positive = down)
//   shell-input tap 270 600          touchscreen tap (logical px)
//   shell-input press 270 600 800    touch and hold for 800 ms
//   shell-input swipe 270 900 270 300 [ms]
//                                    one-finger touch drag (default 300 ms)
//   shell-input sleep 500            wait (ms)
//   shell-input size                 print the logical screen size
// Actions chain in one call: shell-input key super+v sleep 700 key Escape
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const KEYSYMS = {
    super: 0xffeb, super_l: 0xffeb, super_r: 0xffec, ctrl: 0xffe3, control: 0xffe3,
    alt: 0xffe9, shift: 0xffe1, escape: 0xff1b, return: 0xff0d, enter: 0xff0d,
    tab: 0xff09, space: 0x20, backspace: 0xff08, delete: 0xffff,
    up: 0xff52, down: 0xff54, left: 0xff51, right: 0xff53,
    page_up: 0xff55, page_down: 0xff56, home: 0xff50, end: 0xff57,
    print: 0xff61, menu: 0xff67,
};
for (let i = 1; i <= 12; i++)
    KEYSYMS[`f${i}`] = 0xffbe + i - 1;

function keysym(name) {
    const k = name.toLowerCase();
    if (k in KEYSYMS)
        return KEYSYMS[k];
    if (name.length === 1)
        return name.codePointAt(0);   // Latin-1 keysyms equal the character code
    throw new Error(`unknown key "${name}"`);
}

const BUS = 'org.gnome.Mutter.RemoteDesktop';
const rd = Gio.DBusProxy.new_for_bus_sync(Gio.BusType.SESSION,
    Gio.DBusProxyFlags.DO_NOT_LOAD_PROPERTIES, null,
    BUS, '/org/gnome/Mutter/RemoteDesktop', BUS, null);
const [sessionPath] = rd.call_sync('CreateSession', null, 0, -1, null).deepUnpack();
const session = Gio.DBusProxy.new_for_bus_sync(Gio.BusType.SESSION,
    Gio.DBusProxyFlags.DO_NOT_LOAD_PROPERTIES, null,
    BUS, sessionPath, `${BUS}.Session`, null);

function call(method, sig, args) {
    session.call_sync(method, sig ? new GLib.Variant(sig, args) : null, 0, -1, null);
}
const sleep = ms => GLib.usleep(ms * 1000);

// The primary monitor from mutter's DisplayConfig: logical size (physical
// mode size / scale, swapped for a 90 / 270 degree transform), scale and
// connector.
function primaryMonitor() {
    const dc = Gio.DBusProxy.new_for_bus_sync(Gio.BusType.SESSION,
        Gio.DBusProxyFlags.DO_NOT_LOAD_PROPERTIES, null,
        'org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig',
        'org.gnome.Mutter.DisplayConfig', null);
    const [, monitors, logical] = dc.call_sync('GetCurrentState', null, 0, -1, null).deepUnpack();
    const primary = logical.find(l => l[4]) ?? logical[0];
    const [, , scale, transform, , assigned] = primary;
    const [connector] = assigned[0];
    const monitor = monitors.find(m => m[0][0] === connector);
    const mode = monitor[1].find(m => m[6]['is-current']?.deepUnpack()) ?? monitor[1][0];
    let [w, h] = [mode[1] / scale, mode[2] / scale];
    if (transform % 2 === 1)
        [w, h] = [h, w];
    return {width: Math.round(w), height: Math.round(h), scale, connector};
}

function screenSize() {
    const {width, height} = primaryMonitor();
    return [width, height];
}

// Touch needs absolute positions, which mutter only takes relative to a
// screen-cast stream of the remote desktop session: record the primary
// monitor (before Start). Stream coordinates are logical px x scale.
const TOUCH_OPS = ['tap', 'press', 'swipe'];
let stream = null, streamScale = 1;
let touching = false;	// a finger is down: lift it even when an action fails

function setupTouch() {
    const [sessionId] = session.call_sync('org.freedesktop.DBus.Properties.Get',
        new GLib.Variant('(ss)', [`${BUS}.Session`, 'SessionId']), 0, -1, null)
        .deepUnpack().map(v => v.deepUnpack());
    const SC = 'org.gnome.Mutter.ScreenCast';
    const sc = Gio.DBusProxy.new_for_bus_sync(Gio.BusType.SESSION,
        Gio.DBusProxyFlags.DO_NOT_LOAD_PROPERTIES, null,
        SC, '/org/gnome/Mutter/ScreenCast', SC, null);
    const [scPath] = sc.call_sync('CreateSession', new GLib.Variant('(a{sv})',
        [{'remote-desktop-session-id': new GLib.Variant('s', sessionId)}]), 0, -1, null).deepUnpack();
    const scSession = Gio.DBusProxy.new_for_bus_sync(Gio.BusType.SESSION,
        Gio.DBusProxyFlags.DO_NOT_LOAD_PROPERTIES, null, SC, scPath, `${SC}.Session`, null);
    const monitor = primaryMonitor();
    [stream] = scSession.call_sync('RecordMonitor', new GLib.Variant('(sa{sv})',
        [monitor.connector, {}]), 0, -1, null).deepUnpack();
    streamScale = monitor.scale;
}

function touch(method, x, y) {
    call(method, '(sudd)', [stream, 0, x * streamScale, y * streamScale]);
}

function pressKeys(names) {
    const syms = names.split('+').map(keysym);
    for (const s of syms) {
        call('NotifyKeyboardKeysym', '(ub)', [s, true]);
        sleep(30);
    }
    for (const s of syms.reverse()) {
        call('NotifyKeyboardKeysym', '(ub)', [s, false]);
        sleep(30);
    }
}

const argv = [...ARGV];
if (argv.length === 0) {
    printerr('usage: shell-input (key K | move DX DY | moveto X Y | click [B] | scroll DY |\n' +
        '    tap X Y | press X Y MS | swipe X1 Y1 X2 Y2 [MS] | sleep MS | size)...');
    imports.system.exit(1);
}
if (argv.some(a => TOUCH_OPS.includes(a)))
    setupTouch();
call('Start');
try {
    while (argv.length) {
        const op = argv.shift();
        switch (op) {
        case 'key':
            pressKeys(argv.shift());
            break;
        case 'move':
            call('NotifyPointerMotionRelative', '(dd)', [Number(argv.shift()), Number(argv.shift())]);
            break;
        case 'moveto': {
            // Only relative motion is available without a screen-cast stream:
            // park the pointer in the bottom-right corner (the top-left one is
            // the Activities hot corner) and move from there.
            const [w, h] = screenSize();
            call('NotifyPointerMotionRelative', '(dd)', [100000, 100000]);
            // Let the shell process the leave from wherever the pointer was
            // (a panel indicator under it would otherwise switch the open
            // menu when the next motion arrives in the same frame).
            sleep(150);
            call('NotifyPointerMotionRelative', '(dd)',
                [Number(argv.shift()) - (w - 1), Number(argv.shift()) - (h - 1)]);
            break;
        }
        case 'click': {
            const b = /^\d$/.test(argv[0] ?? '') ? Number(argv.shift()) : 1;
            const code = {1: 0x110, 2: 0x112, 3: 0x111}[b];   // BTN_LEFT, BTN_MIDDLE, BTN_RIGHT
            call('NotifyPointerButton', '(ib)', [code, true]);
            sleep(60);
            call('NotifyPointerButton', '(ib)', [code, false]);
            // Park the pointer in the bottom-right corner afterwards: left
            // resting on a panel indicator, the next tap inside a menu opened
            // by key would switch the shell to that indicator's menu.
            sleep(60);
            call('NotifyPointerMotionRelative', '(dd)', [100000, 100000]);
            break;
        }
        case 'scroll':
            call('NotifyPointerAxisDiscrete', '(ui)', [1, Number(argv.shift())]);   // axis 1 = vertical
            break;
        case 'tap':
        case 'press': {
            const [x, y] = [Number(argv.shift()), Number(argv.shift())];
            const hold = op === 'press' ? Number(argv.shift()) : 60;
            touch('NotifyTouchDown', x, y);
            touching = true;
            sleep(hold);
            call('NotifyTouchUp', '(u)', [0]);
            touching = false;
            break;
        }
        case 'swipe': {
            const [x1, y1, x2, y2] = argv.splice(0, 4).map(Number);
            const ms = /^\d+$/.test(argv[0] ?? '') ? Number(argv.shift()) : 300;
            const steps = Math.max(2, Math.round(ms / 16));
            touch('NotifyTouchDown', x1, y1);
            touching = true;
            for (let i = 1; i <= steps; i++) {
                sleep(ms / steps);
                touch('NotifyTouchMotion',
                    x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
            }
            call('NotifyTouchUp', '(u)', [0]);
            touching = false;
            break;
        }
        case 'sleep':
            sleep(Number(argv.shift()));
            break;
        case 'size':
            print(screenSize().join('x'));
            break;
        default:
            throw new Error(`unknown action "${op}"`);
        }
        sleep(30);
    }
} finally {
    // A removed virtual touchscreen does not end its touches: a finger left
    // down stays down in the shell's gestures (the overview got stuck open).
    if (touching)
        call('NotifyTouchUp', '(u)', [0]);
    call('Stop');
}
