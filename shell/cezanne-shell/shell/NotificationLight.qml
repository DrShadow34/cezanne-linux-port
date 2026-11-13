import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Services.Notifications
import Quickshell.Services.UPower

// The notification light (cezanne-notification-led's): feedbackd drives the
// pop-up camera's light (rgb:status, AW2015) and the vibrator (AW8697).
// - A new notification vibrates (feedbackd's notification-new-generic, with
//   the app's ID for feedbackd's per-app profiles).
// - The light fades in and out while there are unread notifications (ones
//   that came since the last unlock or since the calendar's list was last
//   opened, and are not dismissed), in the colour of the newest one's app.
// - Else, on the charger: steady red while charging, green when charged or
//   held at the charge limit.
// Do Not Disturb suppresses both.
// feedbackd's LED feedback has a fixed colour per event: a theme
// (~/.config/feedbackd/themes/cezanne.json, parent "default") with one
// event per colour in use (x-cezanne-led-rrggbb) and x-cezanne-charging /
// -charged, selected in feedbackd's settings. A light state is one such
// event looping in fbcli (timeout 0) until it reads a RETURN. Settings: the
// GNOME extension's dconf keys (colour, per-app colours, vibrate, battery
// light, charged percentage).
Scope {
    id: light
    property var notifications: null        // Notifications.qml
    // the sandbox: a stand-in for fbcli, and the user's theme left alone
    readonly property string fbcli: Quickshell.env("CEZANNE_FBCLI") || "fbcli"
    readonly property bool ownTheme: !Quickshell.env("CEZANNE_FBCLI")
    readonly property string base: "/org/gnome/shell/extensions/cezanne-notification-led/"
    readonly property string themeFile: Quickshell.env("HOME") + "/.config/feedbackd/themes/cezanne.json"

    property string color: "#0000ff"
    property var appColors: ({})
    property bool vibrate: true
    property bool batteryLight: true
    property int chargedPercentage: 95
    property bool ready: false              // settings read, theme in place

    property var lit: []                    // [{ id, color }], oldest first
    property string wanted: ""              // the LED event that should run

    function normalize(c) {
        const m = /^#([0-9a-f]{6})$/i.exec(c ?? "")
        return m ? "#" + m[1].toLowerCase() : ""
    }

    // ---- settings (dconf: GVariant text, empty for the default)
    function readSettings() { settingsRead.running = true }
    Process {
        id: settingsRead
        running: true
        command: ["sh", "-c", 'for k in notification-color app-colors vibrate battery-light charged-percentage; do echo "$k=$(dconf read "$0$k")"; done', light.base]
        stdout: StdioCollector {
            onStreamFinished: {
                const v = {}
                for (const line of text.split("\n")) {
                    const i = line.indexOf("=")
                    if (i > 0) v[line.slice(0, i)] = line.slice(i + 1).trim()
                }
                light.color = light.normalize((v["notification-color"] ?? "").replace(/^'|'$/g, "")) || "#0000ff"
                const ac = {}
                const re = /'([^']+)': '([^']+)'/g
                let m
                while ((m = re.exec(v["app-colors"] ?? "")) !== null)
                    if (light.normalize(m[2])) ac[m[1]] = light.normalize(m[2])
                light.appColors = ac
                light.vibrate = v["vibrate"] !== "false"
                light.batteryLight = v["battery-light"] !== "false"
                light.chargedPercentage = parseInt((v["charged-percentage"] ?? "").replace(/^int32 /, "")) || 95
                light.writeTheme()
            }
        }
    }
    Process {
        running: true
        command: ["dconf", "watch", light.base]
        stdout: SplitParser { onRead: settingsAgain.restart() }
    }
    Timer { id: settingsAgain; interval: 300; onTriggered: light.readSettings() }

    // ---- the feedbackd theme; selecting it again makes feedbackd reload
    function writeTheme() {
        if (!ownTheme) { ready = true; sync(); return }
        const led = (event, c, f) => ({ "event-name": event, type: "Led", color: c, frequency: f })
        const colors = [...new Set([color].concat(Object.values(appColors)))].sort()
        const theme = {
            name: "cezanne",
            "parent-name": "default",
            profiles: [{
                name: "silent",
                feedbacks: [led("x-cezanne-charging", "#ff0000", 0), led("x-cezanne-charged", "#00ff00", 0)]
                    .concat(colors.map(c => led("x-cezanne-led-" + c.slice(1), c, 300)))
            }]
        }
        themeJob.contents = JSON.stringify(theme, null, 2) + "\n"
        themeJob.running = true
    }
    Process {
        id: themeJob
        property string contents
        // rewritten only when it changed; the running light belongs to the
        // old theme then
        command: ["sh", "-c", 'mkdir -p "${0%/*}"; if [ "$(cat "$0" 2>/dev/null)" != "$(printf %s "$1")" ]; then printf %s "$1" > "$0"; echo changed; fi; [ "$(gsettings get org.sigxcpu.feedbackd theme)" = "\'cezanne\'" ] || echo changed',
                  light.themeFile, contents]
        stdout: StdioCollector {
            onStreamFinished: {
                if (text.indexOf("changed") >= 0) {
                    light.wanted = ""
                    if (lightProc.running) lightProc.write("\n")
                    Quickshell.execDetached(["sh", "-c", "gsettings set org.sigxcpu.feedbackd theme default; gsettings set org.sigxcpu.feedbackd theme cezanne"])
                }
                light.ready = true
                light.sync()
            }
        }
    }

    // ---- what the light shows
    readonly property var battery: UPower.displayDevice
    readonly property string batteryEvent: {
        if (!batteryLight || !battery || !battery.isPresent) return ""
        const s = battery.state
        if (s === UPowerDeviceState.FullyCharged || s === UPowerDeviceState.PendingCharge) return "x-cezanne-charged"
        if (s !== UPowerDeviceState.Charging) return ""
        return battery.percentage * 100 >= chargedPercentage ? "x-cezanne-charged" : "x-cezanne-charging"
    }
    onBatteryEventChanged: sync()

    function sync() {
        if (!ready) return
        set(lit.length && !ShellState.dnd ? "x-cezanne-led-" + lit[lit.length - 1].color.slice(1) : batteryEvent)
    }
    function clear() {
        lit = []
        sync()
    }
    Connections {
        target: light.notifications
        function onAdded(n) {
            if (n.urgency === NotificationUrgency.Low || n.transient) return
            const app = n.desktopEntry || n.appName
            const id = n.id
            light.lit = light.lit.filter(l => l.id !== id).concat([{ id: id, color: light.appColors[app] ?? light.color }])
            // dismissed (or expired, or closed by its app): not unread
            n.closed.connect(() => {
                light.lit = light.lit.filter(l => l.id !== id)
                light.sync()
            })
            if (!ShellState.dnd && light.vibrate) buzz.createObject(light, { app: app })
            light.sync()
        }
    }
    Connections {
        target: ShellState
        function onLockedChanged() { if (!ShellState.locked) light.clear() }
        function onDndChanged() { light.sync() }
        // the calendar's list counts as read
        function onPopupChanged() { if (ShellState.popup === "calendar") light.clear() }
    }

    // ---- one light event at a time: end the old one (RETURN to fbcli),
    // then start the new one. Ending writes brightness 0, which drops the
    // LED's pattern trigger until udev (feedbackd's fbd-ledctrl) sets it
    // again; a blink started before that stays steady: wait for it.
    function set(name) {
        if (name === wanted) return
        wanted = name
        if (lightProc.running) lightProc.write("\n")
        else start()
    }
    function start() {
        if (wanted === "" || lightProc.running) return
        lightProc.event = wanted
        lightProc.command = ["sh", "-c", 'for i in $(seq 40); do grep -q "\\[pattern\\]" /sys/class/leds/rgb:status/trigger 2>/dev/null && break; sleep 0.05; done; exec "$0" -E "$1" -t 0 -w 2000000000',
                             fbcli, wanted]
        lightProc.running = true
    }
    Process {
        id: lightProc
        property string event
        stdinEnabled: true
        // the next one; the same one again when it ended by itself
        // (feedbackd restarted), after a pause
        onExited: {
            if (light.wanted === event) restartLight.start()
            else light.start()
        }
    }
    Timer { id: restartLight; interval: 2000; onTriggered: light.start() }

    // ---- the vibration of a new notification (fbcli exits when it ends;
    // its stdin stays open, else it ends the feedback at once)
    Component {
        id: buzz
        Process {
            property string app
            running: true
            stdinEnabled: true
            command: [light.fbcli, "-E", "notification-new-generic"].concat(app !== "" ? ["-A", app] : [])
            onExited: destroy()
        }
    }
}
