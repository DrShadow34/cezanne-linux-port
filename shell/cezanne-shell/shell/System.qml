pragma Singleton
import QtQuick
import Quickshell
import Quickshell.Io

// Things the shell changes on the system: backlight and automatic
// brightness, dark
// style and wallpaper (GNOME's settings, which GTK apps follow), the
// flashlight LEDs, power actions and app launching
Singleton {
    id: sys

    // ---- backlight: written through a small shell that stays open (the
    // video group writes it, cezanne-base's udev rule), else through
    // logind's SetBrightness; at most one write per 40 ms either way
    // (the sandbox: CEZANNE_BACKLIGHT, a directory of its own)
    readonly property string backlightDir: Quickshell.env("CEZANNE_BACKLIGHT") || "/sys/class/backlight"
    property string backlight: ""
    property bool backlightWritable: false
    property int maxBrightness: 1
    property int brightness: 0
    readonly property real brightnessLevel: maxBrightness > 0 ? brightness / maxBrightness : 0
    property int pendingBrightness: -1

    function readBrightness() { if (backlight !== "" && pendingBrightness < 0) blRead.running = true }
    function setBrightnessLevel(level) {
        // never fully dark
        const v = Math.max(1, Math.round(Math.min(1, Math.max(0, level)) * maxBrightness))
        if (v === brightness && pendingBrightness < 0) return
        brightness = v
        pendingBrightness = v
        if (!blWrite.running && !blTimer.running) blTimer.start()
    }

    Process {
        command: ["sh", "-c", 'for b in "$0"/*; do echo "${b##*/} $(cat $b/max_brightness) $(cat $b/brightness) $([ -w $b/brightness ] && echo 1 || echo 0)"; break; done',
                  sys.backlightDir]
        running: true
        stdout: SplitParser {
            onRead: line => {
                const f = line.split(" ")
                if (f.length < 4) return
                sys.backlight = f[0]
                sys.maxBrightness = parseInt(f[1])
                sys.brightness = parseInt(f[2])
                sys.backlightWritable = f[3] === "1"
            }
        }
    }
    Process {
        id: blRead
        command: ["cat", sys.backlightDir + "/" + sys.backlight + "/brightness"]
        stdout: SplitParser { onRead: line => { if (sys.pendingBrightness < 0) sys.brightness = parseInt(line) } }
    }
    Process {
        id: blWriter
        running: sys.backlightWritable
        stdinEnabled: true
        command: ["sh", "-c", 'while read -r v; do echo "$v" > "$0"; done',
                  sys.backlightDir + "/" + sys.backlight + "/brightness"]
    }
    Timer {
        id: blTimer
        interval: 40
        onTriggered: {
            if (sys.pendingBrightness < 0) return
            if (blWriter.running) {
                blWriter.write(sys.pendingBrightness + "\n")
            } else {
                blWrite.command = ["busctl", "call", "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
                                   "org.freedesktop.login1.Session", "SetBrightness", "ssu", "backlight",
                                   sys.backlight, String(sys.pendingBrightness)]
                blWrite.running = true
            }
            sys.pendingBrightness = -1
        }
    }
    Process {
        id: blWrite
        onExited: if (sys.pendingBrightness >= 0 && !blTimer.running) blTimer.start()
    }

    // ---- automatic brightness (cezanne-autobrightness's, in place of
    // gnome-settings-daemon's): the ambient light (iio-sensor-proxy through
    // monitor-sensor) through a lux -> nits curve interpolated in log(lux),
    // brighten after 0.3 s, dim after 2 s, smooth ramps; the slider scales
    // the curve (x1/4 .. x4) instead of shifting it. On / off is GNOME's
    // ambient-enabled key, which sunlight-boost (panel HBM above 1000 lux,
    // cezanne-base) reads too. Not in the AOD (the panel dozes at its own
    // level); a wake takes a fresh reading and jumps to it.
    // The front ALS reads low (0-2 lux under a lamp, ~7 indoors by day):
    // ~3x MIUI's curve below 30 lux, MIUI's in sunlight; 500 nits = full
    // backlight
    readonly property var curve: [[0, 10], [2, 22], [5, 40], [10, 65], [30, 110], [80, 160], [200, 230],
                                  [500, 320], [1000, 410], [2000, 470], [4000, 500]]
    readonly property bool autoForced: Quickshell.env("CEZANNE_AUTO_BRIGHTNESS") === "1"   // the sandbox
    property bool autoBrightness: autoForced
    property real lux: -1
    property real autoGoal: -1
    property real autoLevel: -1
    readonly property real autoGain: Math.pow(4, 2 * ShellState.autoBias - 1)

    function setAutoBrightness(on) {
        autoBrightness = on
        if (!autoForced)
            Quickshell.execDetached(["gsettings", "set", "org.gnome.settings-daemon.plugins.power", "ambient-enabled", on ? "true" : "false"])
    }
    function setAutoBias(v) {
        ShellState.autoBias = Math.min(1, Math.max(0, v))
        autoApply()
    }
    function luxToLevel(l) {
        const f = x => Math.log10(1 + x)
        const x = f(Math.max(0, l))
        for (let i = 1; i < curve.length; i++) {
            const [l0, n0] = curve[i - 1], [l1, n1] = curve[i]
            if (x <= f(l1)) return (n0 + (x - f(l0)) / (f(l1) - f(l0)) * (n1 - n0)) / 500
        }
        return curve[curve.length - 1][1] / 500
    }
    function onLux(l) {
        // changes under 10 % (+1 lux) are noise
        if (lux >= 0 && Math.abs(l - lux) <= 0.1 * lux + 1) return
        lux = l
        const goal = luxToLevel(l)
        if (autoLevel < 0) {        // the first reading: jump
            autoLevel = autoGoal = goal
            autoApply()
            return
        }
        autoDelay.goal = goal
        autoDelay.interval = goal > autoLevel ? 300 : 2000
        autoDelay.restart()
    }
    function autoApply() { if (autoBrightness && autoLevel >= 0) setBrightnessLevel(autoLevel * autoGain) }
    onAutoBrightnessChanged: {
        lux = autoGoal = autoLevel = -1
        autoDelay.stop()
        autoRamp.stop()
    }
    Timer {
        id: autoDelay
        property real goal: 0
        onTriggered: { sys.autoGoal = goal; autoRamp.start() }
    }
    Timer {
        id: autoRamp
        interval: 33
        repeat: true
        onTriggered: {
            const up = sys.autoGoal > sys.autoLevel
            const a = 1 - Math.exp(-interval / (up ? 250 : 1500))
            let l = sys.autoLevel + (sys.autoGoal - sys.autoLevel) * a
            if (Math.abs(sys.autoGoal - l) < 0.002) { l = sys.autoGoal; stop() }
            sys.autoLevel = l
            sys.autoApply()
        }
    }
    Process {
        id: light
        running: sys.autoBrightness && !ShellState.aodView && sys.backlight !== ""
        // (the sandbox: CEZANNE_LIGHT_MONITOR, iio-sensor-proxy lets only
        // the active session claim the sensor)
        command: Quickshell.env("CEZANNE_LIGHT_MONITOR") ? [Quickshell.env("CEZANNE_LIGHT_MONITOR")]
                                                         : ["monitor-sensor", "--light"]
        // "=== Has ambient light sensor (value: 5.000000, unit: lux)",
        // "    Light changed: 7.000000 (lux)"
        stdout: SplitParser {
            onRead: line => {
                const m = line.match(/(?:value: |Light changed: )([0-9.]+)/)
                if (m) sys.onLux(parseFloat(m[1]))
            }
        }
        onRunningChanged: if (running) { sys.lux = sys.autoGoal = sys.autoLevel = -1; autoRamp.stop(); autoDelay.stop() }
    }
    Process {
        running: !sys.autoForced
        command: ["gsettings", "get", "org.gnome.settings-daemon.plugins.power", "ambient-enabled"]
        stdout: StdioCollector { onStreamFinished: sys.autoBrightness = text.trim() === "true" }
    }
    Process {
        id: autoWatch
        running: !sys.autoForced
        command: ["gsettings", "monitor", "org.gnome.settings-daemon.plugins.power", "ambient-enabled"]
        stdout: SplitParser {
            onRead: line => {
                const on = /true\s*$/.test(line)
                if (on !== sys.autoBrightness) sys.autoBrightness = on
            }
        }
    }

    // ---- dark style and wallpaper (GNOME's keys: GTK / libadwaita apps
    // follow them through xdg-desktop-portal-gnome)
    property bool dark: false
    property string wallpaper: ""
    property string wallpaperLight: ""
    property string wallpaperDark: ""
    function setDark(on) {
        dark = on
        Quickshell.execDetached(["gsettings", "set", "org.gnome.desktop.interface", "color-scheme", on ? "prefer-dark" : "default"])
    }
    onDarkChanged: wallpaper = (dark && wallpaperDark !== "") ? wallpaperDark : wallpaperLight

    // the lock screen's background: the wallpaper blurred once (ffmpeg) into
    // the cache, instead of a live full-screen blur
    property string lockWallpaper: ""
    onWallpaperChanged: {
        lockWallpaper = ""
        if (wallpaper === "") return
        const out = Quickshell.cachePath("lock-" + Qt.md5(wallpaper) + ".png")
        blur.command = ["sh", "-c", `[ -s "$2" ] || ffmpeg -loglevel error -y -i "$1" -vf "scale=540:-2,gblur=sigma=14" "$2"; [ -s "$2" ] && echo "$2"`,
                        "sh", wallpaper, out]
        blur.running = true
    }
    Process {
        id: blur
        stdout: SplitParser { onRead: line => sys.lockWallpaper = line }
    }

    Process {
        running: true
        command: ["sh", "-c", "gsettings get org.gnome.desktop.interface color-scheme; gsettings get org.gnome.desktop.background picture-uri; gsettings get org.gnome.desktop.background picture-uri-dark"]
        stdout: StdioCollector {
            onStreamFinished: {
                const l = text.split("\n").map(s => s.replace(/^'|'$/g, ""))
                sys.wallpaperLight = (l[1] ?? "").replace(/^file:\/\//, "")
                sys.wallpaperDark = (l[2] ?? "").replace(/^file:\/\//, "")
                sys.dark = l[0] === "prefer-dark"
                sys.wallpaper = (sys.dark && sys.wallpaperDark !== "") ? sys.wallpaperDark : sys.wallpaperLight
            }
        }
    }

    // ---- flashlight: the rear flash LEDs (/sys/class/leds/*:flash-*,
    // writable for the user as with the GNOME extension)
    property var flashLeds: []
    property bool flashlight: false
    readonly property var flashLevels: [0.25, 0.5, 1.0]
    function setFlashlight(on) {
        flashlight = on
        const pct = Math.round(ShellState.flashLevel * 100)
        const cmd = flashLeds.map(l => `echo ${on ? `$(( ($(cat ${l}/max_brightness) * ${pct} + 99) / 100 ))` : 0} > ${l}/brightness`).join("; ")
        if (cmd !== "") Quickshell.execDetached(["sh", "-c", cmd])
    }
    Process {
        running: true
        command: ["sh", "-c", "for l in /sys/class/leds/*:flash-*; do [ -e \"$l/brightness\" ] && echo \"$l $(cat $l/brightness)\"; done"]
        stdout: StdioCollector {
            onStreamFinished: {
                const leds = [], on = []
                for (const line of text.split("\n")) {
                    const f = line.split(" ")
                    if (f.length < 2) continue
                    leds.push(f[0])
                    on.push(parseInt(f[1]) > 0)
                }
                sys.flashLeds = leds
                sys.flashlight = on.some(x => x)
            }
        }
    }

    // ---- panel (cezanne-dc-dimming's): DC dimming (the driver keeps the
    // panel out of its low PWM range and dims the pixels instead) and the
    // colour scheme (the panel's colour tables, MIUI's names). Kept in
    // ~/.config/cezanne/ as with the GNOME extension, applied at start. The
    // attributes run DCS sequences (~60 ms): one write at a time, in order.
    // (the sandbox: CEZANNE_PANEL, CEZANNE_CONFIG point elsewhere)
    readonly property string panel: Quickshell.env("CEZANNE_PANEL") || "/sys/bus/mipi-dsi/devices/1400e000.dsi.0"
    readonly property string config: Quickshell.env("CEZANNE_CONFIG") || Quickshell.env("HOME") + "/.config/cezanne"
    readonly property var colorModes: [["p3", "Vivid"], ["p3-d65", "Natural"], ["srgb", "Standard"], ["native", "Saturated"]]
    property bool dcDimming: false
    property string colorMode: "p3"
    property var panelWrites: []
    function setDcDimming(on) {
        dcDimming = on
        panelWrite("dc_dimming", "dc-dimming", on ? "1" : "0")
    }
    function setColorMode(mode) {
        if (!colorModes.some(c => c[0] === mode)) mode = "p3"
        colorMode = mode
        panelWrite("color_mode", "color-mode", mode)
    }
    function panelWrite(attr, file, value) {
        panelWrites = panelWrites.concat([[attr, file, value]])
        panelNext()
    }
    function panelNext() {
        if (panelJob.running || panelWrites.length === 0) return
        const [attr, file, value] = panelWrites[0]
        panelWrites = panelWrites.slice(1)
        panelJob.command = ["sh", "-c", 'echo "$3" > "$0/$1"; mkdir -p "$4" && echo "$3" > "$4/$2"',
                            panel, attr, file, value, config]
        panelJob.running = true
    }
    Process {
        id: panelJob
        onExited: sys.panelNext()
    }
    Process {
        running: true
        command: ["sh", "-c", 'cat "$0/dc-dimming" 2>/dev/null || echo 0; cat "$0/color-mode" 2>/dev/null || echo p3', config]
        stdout: StdioCollector {
            onStreamFinished: {
                const l = text.split("\n")
                sys.setDcDimming(l[0].trim() === "1")
                sys.setColorMode((l[1] ?? "").trim())
            }
        }
    }

    // ---- power
    function suspend() { Quickshell.execDetached(["systemctl", "suspend"]) }
    function reboot() { Quickshell.execDetached(["systemctl", "reboot"]) }
    function poweroff() { Quickshell.execDetached(["systemctl", "poweroff"]) }
    function logout() { Quickshell.execDetached(["niri", "msg", "action", "quit", "--skip-confirmation"]) }

    // ---- apps: started by niri (each in its own systemd scope), not as
    // children of the shell
    function commandOf(entry) {
        let cmd = entry.execString.replace(/%[fFuUdDnNickvm]/g, "").trim()
        // GNOME Settings refuses to start outside GNOME
        if (/^(\S*\/)?gnome-control-center\b/.test(cmd)) cmd = "env XDG_CURRENT_DESKTOP=GNOME " + cmd
        return cmd
    }
    function launch(entry) {
        if (!entry) return
        Niri.spawnSh(commandOf(entry))
    }
    function launchId(id) { launch(DesktopEntries.byId(id)) }
    function openSettings(panel) {
        Niri.spawnSh("env XDG_CURRENT_DESKTOP=GNOME gnome-control-center" + (panel ? " " + panel : ""))
    }

    // the desktop entry of a window's app id
    function entryFor(appId) {
        if (!appId) return null
        return DesktopEntries.byId(appId) ?? DesktopEntries.heuristicLookup(appId)
    }
    // the app list: bindings that look entries up read it, so they look
    // again when it (re)loads (it comes in after the shell starts)
    readonly property var apps: DesktopEntries.applications.values
    function iconFor(entry, fallback) {
        const fb = fallback ?? "application-x-executable"
        const name = entry?.icon ?? ""
        if (name.startsWith("/")) return "file://" + name
        return Quickshell.iconPath(name !== "" ? name : fb, fb)
    }
}
