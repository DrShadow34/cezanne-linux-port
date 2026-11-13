import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import Quickshell.Networking
import Quickshell.Bluetooth

// The always-on display (cezanne-aod in GNOME, the same way): the panel has
// a doze mode, in which it shows its frame memory at a fixed low brightness
// by itself, through crtc disable and suspend. So: lock, fade the lock
// surface's AOD view to black, switch the panel to doze (its brightness
// change unseen on black), fade the clock in, let that frame out, arm the
// RTC for the next minute and sleep. At
// the alarm: move the clock (burn-in), redraw, sleep again. Woken by the
// power key or the charger: leave doze, the lock screen is there. Woken by
// anything else (Wi-Fi, Bluetooth): awake a moment, sleep again, still in
// doze. It sleeps by itself (/sys/power/state, cezanne-aod-ctl), not through
// logind, and takes the power key from logind while it is on.
// Settings: cezanne-power's (org.cezanne.power).
Scope {
    id: aod
    // CEZANNE_AOD_CTL: a stand-in for tests (test/aod-ctl-dummy)
    readonly property string ctl: Quickshell.env("CEZANNE_AOD_CTL") || "/usr/lib/cezanne-shell/cezanne-aod-ctl"

    // settings
    property bool enabled: true
    property string brightnessMode: "auto"     // auto / low / high
    property string wifiStandby: "connected"   // connected / off / after N minutes
    property int wifiStandbyMinutes: 15
    property bool btStandby: true

    property bool active: false     // AOD view up, panel dozing
    property bool entering: false
    property bool sleeping: false   // cezanne-aod-ctl sleep running
    property int alarm: 0           // epoch s of the armed RTC alarm
    property int minute: -1         // the minute the clock shows
    property real wokeAt: 0         // ms of the last wake to the lock screen
    property real deepRefused: 0    // ms of the last refused deep attempt
    property real aodSince: 0       // ms the AOD came up
    property bool deep: false
    property bool wifiOff: false    // turned off here for standby
    property bool btOff: false
    property string luxLevel: "1"
    property real luxAt: 0
    property int generation: 0      // bumped on exit: stale steps stop

    readonly property int alarmSlack: 3       // s: woken this close = RTC wake
    readonly property int brightLux: 50       // doze bright above this light
    readonly property int luxEvery: 300       // s between light reads
    readonly property int wokenMs: 1500       // the press that woke it
    readonly property string pmicIrq: "mt6358-irq"  // power key, charger, RTC

    // ---- settings and the power key
    function setting(line) {
        const m = line.match(/^([a-z-]+):?\s+(.*)$/)
        if (!m) return
        const v = m[2].replace(/^uint32 /, "").replace(/^'|'$/g, "")
        switch (m[1]) {
        case "aod-enabled": enabled = v === "true"; break
        case "aod-brightness": brightnessMode = v; break
        case "wifi-standby": wifiStandby = v; break
        case "wifi-standby-minutes": wifiStandbyMinutes = parseInt(v); break
        case "bluetooth-standby": btStandby = v === "true"; break
        }
    }
    Process {
        running: true
        command: ["gsettings", "list-recursively", "org.cezanne.power"]
        stdout: SplitParser { onRead: line => aod.setting(line.replace(/^org\.cezanne\.power /, "")) }
    }
    Process {
        id: monitor
        running: true
        command: ["gsettings", "monitor", "org.cezanne.power"]
        stdout: SplitParser { onRead: line => aod.setting(line) }
        onExited: monitorRestart.start()
    }
    Timer { id: monitorRestart; interval: 5000; onTriggered: monitor.running = true }
    function setEnabled(on) {
        Quickshell.execDetached(["gsettings", "set", "org.cezanne.power", "aod-enabled", on ? "true" : "false"])
    }

    // logind leaves the power key to the shell while the AOD is on (niri
    // binds XF86PowerOff to `cezanne-shell-ctl aod power`); off, logind's
    // HandlePowerKey=suspend acts again
    Process {
        running: aod.enabled
        command: ["systemd-inhibit", "--what=handle-power-key", "--mode=block", "--who=cezanne-shell",
                  "--why=Always-on display", "sleep", "infinity"]
    }

    // ---- small async helpers
    Component {
        id: job
        Process {
            id: p
            property var done: null
            property int code: -1
            property bool exited: false
            property bool read: false
            function finish() {
                if (!exited || !read) return
                const f = done
                const t = out.text
                destroy()
                if (f) f(code, t)
            }
            stdout: StdioCollector {
                id: out
                onStreamFinished: { p.read = true; p.finish() }
            }
            onExited: c => { p.code = c; p.exited = true; p.finish() }
        }
    }
    function run(args) {
        return new Promise(resolve => {
            const p = job.createObject(aod, { command: [ctl].concat(args), done: (code, text) => resolve({ code: code, text: text }) })
            p.running = true
        })
    }
    Component {
        id: timer
        Timer {
            property var done
            onTriggered: { const f = done; destroy(); f() }
        }
    }
    function delay(ms) {
        return new Promise(resolve => timer.createObject(aod, { interval: Math.max(1, ms), done: resolve }).start())
    }
    // 50 ms after the lock surface's next frame is out (or after 1 s: no
    // frames while the display is off)
    function framePresented() {
        const start = ShellState.lockFrames
        return new Promise(resolve => {
            let left = 60
            const poll = () => {
                if (ShellState.lockFrames > start || --left <= 0) delay(50).then(resolve)
                else delay(16).then(poll)
            }
            poll()
        })
    }
    function lockFirst() {
        return new Promise(resolve => {
            if (!ShellState.locked) ShellState.lock()
            let left = 150
            const poll = () => {
                if (ShellState.lockSecure || --left <= 0) resolve(ShellState.lockSecure)
                else delay(20).then(poll)
            }
            poll()
        })
    }

    // ---- entry points (IPC from niri's power key bind and swayidle; the
    // locked-idle monitor; the power menu)
    function powerKey() {
        if (!enabled) return                       // logind suspends
        if (Date.now() - wokeAt < wokenMs) return  // the press that woke it
        if (active) wake()
        else goSleep()
    }
    function idle() {
        if (enabled) goSleep()
        else plainSuspend(true)
    }
    function plainSuspend(blank) {
        lockFirst().then(() => {
            if (blank) Niri.monitorsOff()
            return delay(blank ? 5000 : 0)
        }).then(() => {
            if (ShellState.locked && !ShellState.caffeine) Quickshell.execDetached(["systemctl", "suspend"])
        })
    }
    Connections {
        target: ShellState
        function onSleepRequested() { if (aod.enabled) aod.goSleep(); else Quickshell.execDetached(["systemctl", "suspend"]) }
    }
    // locked and left alone: to sleep (into the AOD) before the screen
    // sits lit
    IdleMonitor {
        enabled: ShellState.locked && !aod.active && !aod.entering
        timeout: 20
        respectInhibitors: true
        onIsIdleChanged: if (isIdle) { if (aod.enabled) aod.goSleep(); else aod.plainSuspend(false) }
    }

    // ---- into the AOD
    function goSleep() {
        if (active || entering) return
        entering = true
        const gen = generation
        lockFirst().then(secure => {
            if (gen !== generation) return
            if (!secure) { entering = false; return }
            return open(gen)
        })
    }

    // the panel switches to its doze brightness while the screen is black,
    // then the clock fades in already dimmed (it never shows at the normal
    // level first); the panel keeps doze through new frames
    function open(gen) {
        Niri.action(["power-on-monitors"])
        const t = showTime()
        layout(t)
        // the light reading (~0.4 s) runs during the fade to black
        const level = dozeLevel(true)
        ShellState.aodClock = false
        ShellState.aodView = true                  // fades to black (350 ms)
        return delay(380).then(() => framePresented())
        .then(() => level)
        .then(level => run(["doze", level]).then(r => {
            if (r.code === 0) return true
            // the display went off meanwhile: on again, then doze
            Niri.action(["power-on-monitors"])
            return framePresented().then(() => run(["doze", level])).then(r2 => r2.code === 0)
        }))
        .then(dozing => {
            entering = false
            if (gen !== generation) return
            active = dozing
            if (!dozing) {
                // no doze: plain sleep, any wake shows the lock screen
                exit()
                Quickshell.execDetached(["systemctl", "suspend"])
                return
            }
            if (!aodSince) aodSince = Date.now()
            ShellState.aodClock = true             // the clock fades in (400 ms)
            return delay(420).then(() => framePresented()).then(() => {
                if (gen !== generation) return
                arm(t)
                kernelSleep()
            })
        })
    }

    // ---- sleeping and waking
    Timer {
        id: restTimer
        onTriggered: aod.update().then(() => { if (aod.active && !restTimer.running) aod.kernelSleep() })
    }
    function rest(ms) {
        restTimer.interval = Math.max(1, ms)
        restTimer.restart()
    }

    function kernelSleep() {
        if (!active || sleeping || restTimer.running) return
        const gen = generation
        run(["power"]).then(r => {
            if (gen !== generation || !active) return
            const [vbus, , , blocked] = r.text.trim().split(" ")
            // a sleep block inhibitor, or Caffeine: awake in the AOD
            if (blocked === "1" || ShellState.caffeine) {
                rest(5000)
                return
            }
            standby()
            // deep (SPM, ~85 mA vs ~150 mA s2idle) on battery only: with VBUS
            // mtu3 refuses it; refused without a wake interrupt: s2idle for
            // a minute
            deep = vbus !== "1" && Date.now() - deepRefused > 60000
            sleeping = true
            return run(["sleep", deep ? "deep" : "s2idle"]).then(s => {
                sleeping = false
                const [slept, irq, action] = s.text.trim().split(" ")
                if (gen === generation) onWake(slept === "1", irq ?? "0", action ?? "none")
            })
        })
    }

    function onWake(slept, irq, action) {
        if (!active) return
        if (!slept && irq === "0") {
            if (deep) deepRefused = Date.now()
            rest(1000)
            return
        }
        const now = Date.now() / 1000
        const rtc = alarm && Math.abs(now - alarm) <= alarmSlack
        if (action === pmicIrq && !rtc) wake()
        else rest(rtc ? 0 : 1000)
    }

    // out of the AOD to the lock screen
    function wake() {
        wokeAt = Date.now()
        restTimer.stop()
        exit()
    }

    function exit() {
        generation++
        entering = false
        if (alarm) run(["alarm", "0"])
        alarm = 0
        wifiResume()
        btResume()
        ShellState.aodView = false
        ShellState.aodClock = false
        if (!active) return
        active = false
        // once the lock screen's frame is out; a doze left on keeps the
        // panel dim and blocks brightness changes
        Niri.action(["power-on-monitors"])
        const tryLeave = n => framePresented().then(() => run(["doze", "0"])).then(r => {
            if (r.code !== 0 && n > 1 && !active) { Niri.action(["power-on-monitors"]); return tryLeave(n - 1) }
        })
        tryLeave(3)
    }
    // unlocked under the AOD (loginctl unlock-session)
    Connections {
        target: ShellState
        function onLockedChanged() { if (!ShellState.locked && (aod.active || aod.entering)) aod.exit() }
    }

    // ---- the clock
    // the RTC and the system clock drift apart a little: woken for a minute
    // update the system may still be a moment before it; show the alarm's
    function showTime() {
        const now = Math.floor(Date.now() / 1000)
        return alarm - now <= alarmSlack ? Math.max(now, alarm) : now
    }
    function layout(t) {
        minute = Math.floor(t / 60)
        ShellState.aodTime = new Date(t * 1000)
        // a new spot every minute (burn-in)
        ShellState.aodSpot = [Math.random(), Math.random()]
        run(["power"]).then(r => {
            const [, cap, kind] = r.text.trim().split(" ")
            const charging = { charging: " · charging", fast: " · fast charging", direct: " · turbo charging" }[kind] ?? ""
            ShellState.aodBattery = cap ? cap + "%" + charging : ""
        })
    }
    // awake in the AOD: a new minute moves the clock, then the next alarm
    function update() {
        const t = showTime()
        if (Math.floor(t / 60) === minute) {
            arm(t)
            return Promise.resolve()
        }
        const gen = generation
        Niri.action(["power-on-monitors"])
        layout(t)
        return framePresented().then(() => {
            if (gen !== generation || !active) return
            return dozeLevel(false).then(level => run(["doze", level]))
        }).then(() => { if (gen === generation) arm(t) })
    }
    // RTC alarm 1 s into the minute after t, at least 2 s ahead
    function arm(t) {
        const now = Math.floor(Date.now() / 1000)
        let at = t - t % 60 + 61
        if (at < now + 2) at += 60
        if (at === alarm) return    // still armed (woken by something else)
        alarm = at
        run(["alarm", String(at)])
    }
    function dozeLevel(fresh) {
        if (brightnessMode !== "auto") return Promise.resolve(brightnessMode === "high" ? "2" : "1")
        const now = Date.now() / 1000
        if (!fresh && now - luxAt < luxEvery) return Promise.resolve(luxLevel)
        return run(["lux"]).then(r => {
            luxLevel = parseInt(r.text) > brightLux ? "2" : "1"
            luxAt = now
            return luxLevel
        })
    }

    // ---- radios in standby (cezanne-power): Wi-Fi off in the AOD at once
    // or after a while, Bluetooth off unless a device is connected; back on
    // when the AOD ends. A radio the user turned off stays off.
    function standby() {
        if (wifiStandby !== "connected" && !wifiOff && aodSince) {
            const after = wifiStandby === "off" ? 0 : wifiStandbyMinutes * 60000
            if (Date.now() - aodSince >= after && Networking.wifiEnabled) {
                Networking.wifiEnabled = false
                wifiOff = true
            }
        }
        const a = Bluetooth.defaultAdapter
        if (btStandby && !btOff && a && a.enabled && !Bluetooth.devices.values.some(d => d.connected)) {
            a.enabled = false
            btOff = true
        }
    }
    function wifiResume() {
        aodSince = 0
        if (!wifiOff) return
        wifiOff = false
        Networking.wifiEnabled = true
    }
    function btResume() {
        if (!btOff) return
        btOff = false
        if (Bluetooth.defaultAdapter) Bluetooth.defaultAdapter.enabled = true
    }

    // a shell that went away in the AOD left the panel dozing and an alarm
    Component.onCompleted: { run(["doze", "0"]); run(["alarm", "0"]) }
}
