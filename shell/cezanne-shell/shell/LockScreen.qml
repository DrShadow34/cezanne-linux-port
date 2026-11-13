import QtQuick
import QtQuick.Shapes
import QtQuick.Window
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import Quickshell.Services.Pam
import Quickshell.Services.UPower

// The lock screen (ext-session-lock): clock, date, battery and the number
// of notifications over the dimmed wallpaper; swipe up or tap for the
// password prompt, checked through PAM (/etc/pam.d/cezanne-shell); the
// fingerprint spot over the under-display sensor (cezanne-fp)
Scope {
    id: root
    property var notifications: null
    property bool prompting: false
    property string message: ""
    property bool messageIsError: false
    property bool busy: false
    readonly property bool secure: lock.secure

    Connections {
        target: ShellState
        function onLockedChanged() {
            if (ShellState.locked) {
                ShellState.close()
                root.prompting = false
                root.message = ""
            }
        }
    }

    function showPrompt() { if (ShellState.locked) prompting = true }

    function tryUnlock(password) {
        if (pam.active) return
        busy = true
        message = ""
        pam.pending = password
        pam.start()
    }

    PamContext {
        id: pam
        config: "cezanne-shell"
        property string pending: ""
        onPamMessage: {
            if (responseRequired) {
                respond(pending)
                pending = ""
            } else if (message !== "") {
                root.message = message
                root.messageIsError = messageIsError
            }
        }
        onCompleted: result => {
            root.busy = false
            if (result === PamResult.Success) {
                root.prompting = false
                root.hideOsk()
                ShellState.locked = false
            } else {
                root.message = result === PamResult.MaxTries ? "Too many attempts" : "Wrong password"
                root.messageIsError = true
            }
        }
        onError: err => {
            root.busy = false
            root.message = "Authentication error: " + PamError.toString(err)
            root.messageIsError = true
        }
    }

    // ---- squeekboard stays down while locked: the lock screen has its own
    // keyboard, and Qt's text-input state (its idea of the focused surface
    // goes stale across the shell's layer surfaces) enabled the lock
    // surface's input now and then, so squeekboard came up under the lock
    // screen and flashed on unlock. Hidden at the lock and whenever it
    // shows while locked.
    function hideOsk() {
        Quickshell.execDetached(["busctl", "--user", "call", "sm.puri.OSK0", "/sm/puri/OSK0",
                                 "sm.puri.OSK0", "SetVisible", "b", "false"])
    }
    Process {
        running: ShellState.locked
        command: ["gdbus", "monitor", "--session", "--dest", "sm.puri.OSK0", "--object-path", "/sm/puri/OSK0"]
        stdout: SplitParser { onRead: line => { if (line.indexOf("'Visible': <true>") >= 0) root.hideOsk() } }
        onRunningChanged: if (running) root.hideOsk()
    }

    // ---- fingerprint: pam_fprintd (/etc/pam.d/cezanne-shell-fingerprint) on
    // cezanne-fpd, which watches the touchscreen for a finger on the sensor
    // spot itself and publishes org.cezanne.Fod State on the system bus (0
    // off, 1 waiting, 2 pressed: it captures once the panel is in FOD HBM).
    // Live while the lock screen shows: not in the AOD, not while the
    // password keyboard is up (its keys lie over the spot). After a failed
    // run (5 tries, no reader) only the password, until the next wake.
    // the sandbox (test/sandbox.sh): no PAM conversation (CEZANNE_FP_PAM=off),
    // a State timeline (CEZANNE_FOD_MONITOR), a file for the panel's hbm
    readonly property bool fpEnabled: Quickshell.env("CEZANNE_FP_PAM") !== "off"
    readonly property string fodMonitor: Quickshell.env("CEZANNE_FOD_MONITOR") ?? ""
    readonly property string hbmPath: Quickshell.env("CEZANNE_HBM") || "/sys/bus/mipi-dsi/devices/1400e000.dsi.0/hbm"
    property int fodState: 0
    property bool fpSeen: false     // the service waited for a finger in this lock
    readonly property bool fpUsable: fpEnabled && fpSeen && !fpBlocked
    onFodStateChanged: if (fodState > 0) fpSeen = true
    property bool fpBlocked: false
    property string fpMessage: ""
    readonly property bool fpWanted: fpEnabled && ShellState.locked && lock.secure && !ShellState.aodView
                                     && !root.prompting && !root.fpBlocked
    onFpWantedChanged: {
        if (fpWanted) {
            fpArm.restart()
        } else {
            fpArm.stop()
            if (fpPam.active) {
                fpPam.aborting = true
                fpPam.abort()
            }
        }
    }
    Connections {
        target: ShellState
        function onAodViewChanged() { if (ShellState.aodView) root.fpBlocked = false }
        function onLockedChanged() {
            root.fpBlocked = false
            root.fpSeen = false
            if (ShellState.locked) System.readBrightness()
            else root.hbm(false)
        }
    }
    Timer {
        id: fpArm
        interval: 400
        onTriggered: if (root.fpWanted && !fpPam.active) { fpPam.aborting = false; fpPam.start() }
    }
    PamContext {
        id: fpPam
        config: "cezanne-shell-fingerprint"
        property bool aborting: false
        onPamMessage: {
            if (messageIsError && message !== "") {
                root.fpMessage = message
                fpMessageTimer.restart()
            }
        }
        onCompleted: result => {
            if (aborting) return
            if (result === PamResult.Success) {
                root.prompting = false
                root.hideOsk()
                ShellState.locked = false
            } else {
                root.fpBlocked = true
                root.fpMessage = result === PamResult.MaxTries ? "Too many attempts, use the password" : ""
                fpMessageTimer.restart()
            }
        }
        onError: err => {
            if (aborting) return
            console.warn("fingerprint:", PamError.toString(err))
            root.fpBlocked = true
        }
    }
    Timer { id: fpMessageTimer; interval: 2500; onTriggered: root.fpMessage = "" }

    Process {
        id: fodWatch
        running: ShellState.locked
        command: root.fodMonitor !== "" ? [root.fodMonitor]
               : ["gdbus", "monitor", "--system", "--dest", "net.reactivated.Fprint",
                  "--object-path", "/org/cezanne/Fod"]
        stdout: SplitParser {
            onRead: line => {
                const m = line.match(/'State': <uint32 (\d+)>/)
                if (m) root.fodState = parseInt(m[1])
            }
        }
        onRunningChanged: if (!running) root.fodState = 0
    }

    // the press: the screen dims to where FOD HBM brings it back to the
    // current brightness, the disc over the sensor gets the full HBM
    // output; HBM goes on once that frame is out (the service's "UI ready")
    // and off when the press ends (3 s at most)
    readonly property bool pressing: fodState === 2 && ShellState.locked && !ShellState.aodView && !prompting
    property bool hbmWanted: false
    property bool hbmOn: false
    property string hbmSaved: "0"
    // black alpha that maps HBM output back to the current brightness (500
    // nits at full backlight, ~650 in FOD HBM; blending on gamma-encoded
    // values, luminance ~ value^2.2)
    readonly property real dimAlpha: 1 - Math.pow(Math.min(1, 500 * System.brightnessLevel / 650), 1 / 2.2)
    onPressingChanged: {
        hbmWanted = pressing
        if (pressing) pressTimeout.restart()
        else hbm(false)
    }
    Timer { id: pressTimeout; interval: 3000; onTriggered: { root.hbmWanted = false; root.hbm(false) } }
    function frameOut() { if (hbmWanted && !hbmOn) hbm(true) }
    // on: the previous mode (1: sunlight boost) is kept for off. One write
    // at a time, the last state wins (a short press must not leave it on).
    function hbm(on) {
        if (on === hbmOn) return
        hbmOn = on
        hbmSync()
    }
    function hbmSync() {
        if (hbmJob.running) return
        const on = hbmOn
        hbmJob.applied = on
        hbmJob.command = on ? ["sh", "-c", "cat $0; echo 2 > $0", hbmPath]
                            : ["sh", "-c", "echo $1 > $0", hbmPath, hbmSaved === "1" ? "1" : "0"]
        hbmJob.running = true
    }
    Process {
        id: hbmJob
        property bool applied: false
        stdout: StdioCollector {
            onStreamFinished: { const v = text.trim(); if (v !== "" && v !== "2") root.hbmSaved = v }
        }
        onExited: if (applied !== root.hbmOn) root.hbmSync()
    }

    // back to the clock after a while without input
    Timer {
        id: idle
        interval: 30000
        running: root.prompting && !root.busy
        onTriggered: root.prompting = false
    }

    Binding { target: ShellState; property: "lockSecure"; value: lock.secure }
    // a wake from the AOD comes back to the clock page, not to an old prompt
    Connections {
        target: ShellState
        function onAodViewChanged() { if (ShellState.aodView) root.prompting = false }
    }

    WlSessionLock {
        id: lock
        locked: ShellState.locked

        WlSessionLockSurface {
            id: surface
            color: "black"

            SystemClock { id: clock; precision: SystemClock.Minutes }

            Image {
                anchors.fill: parent
                source: System.lockWallpaper !== "" ? "file://" + System.lockWallpaper
                      : System.wallpaper !== "" ? "file://" + System.wallpaper : ""
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
            }
            // light shade for the clock; darker under the keyboard
            Rectangle {
                anchors.fill: parent
                color: "black"
                opacity: root.prompting ? 0.45 : 0.18
                Behavior on opacity { NumberAnimation { duration: 200 } }
            }

            // ---- clock
            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                y: root.prompting ? parent.height * 0.08 : parent.height * 0.18
                Behavior on y { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
                spacing: 4
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: Qt.formatTime(clock.date, "HH:mm")
                    color: Theme.fg
                    font.family: Theme.font
                    font.pixelSize: root.prompting ? 56 : 84
                    font.weight: Font.Light
                    Behavior on font.pixelSize { NumberAnimation { duration: 220 } }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: Qt.formatDate(clock.date, "dddd, d MMMM")
                    color: Theme.fg
                    font.family: Theme.font
                    font.pixelSize: 18
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    readonly property var bat: UPower.displayDevice
                    visible: bat?.isPresent ?? false
                    text: Math.round((bat?.percentage ?? 0) * 100) + "%"
                          + (bat?.state === UPowerDeviceState.Charging ? " · charging"
                             : bat?.state === UPowerDeviceState.FullyCharged ? " · charged" : "")
                    color: Theme.fgDim
                    font.family: Theme.font
                    font.pixelSize: Theme.textSize
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    topPadding: 18
                    visible: !root.prompting && (root.notifications?.count ?? 0) > 0
                    text: root.notifications ? root.notifications.count + (root.notifications.count === 1 ? " notification" : " notifications") : ""
                    color: Theme.fgDim
                    font.family: Theme.font
                    font.pixelSize: Theme.textSize
                }
            }

            // ---- swipe up / tap
            MouseArea {
                anchors.fill: parent
                enabled: !root.prompting
                property real startY: 0
                onPressed: mouse => startY = mouse.y
                onReleased: mouse => root.prompting = true
            }
            Column {
                visible: !root.prompting
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 36
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 6
                SymIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    size: 18
                    name: "go-up-symbolic"
                    color: Theme.fgDim
                }
                Text {
                    text: "Swipe up to unlock"
                    color: Theme.fgDim
                    font.family: Theme.font
                    font.pixelSize: Theme.textSize
                }
            }

            // ---- fingerprint spot: the sensor in panel pixels (MIUI's box
            // (439,1732), 202 px); the lock screen is portrait (Rotation.qml)
            Item {
                id: fod
                readonly property real s: surface.width / 1080
                x: 439 * s
                y: 1732 * s
                width: 202 * s
                height: width
                visible: root.fodState > 0 && !root.prompting && surface.width < surface.height
                // a touch on the spot is the sensor's, not the swipe area's
                MouseArea { anchors.fill: parent }
                SymIcon {
                    anchors.centerIn: parent
                    visible: root.fodState === 1
                    size: Math.round(parent.width * 0.7)
                    name: "auth-fingerprint-symbolic"
                    color: "white"
                }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                y: fod.y - height - 14
                visible: root.fpMessage !== "" && !root.prompting
                text: root.fpMessage
                color: Theme.fg
                font.family: Theme.font
                font.pixelSize: Theme.textSize
            }
            // the press: dim layer, then the disc fading into it
            Item {
                id: press
                anchors.fill: parent
                z: 9
                visible: root.pressing && fod.visible
                readonly property real cx: fod.x + fod.width / 2
                readonly property real cy: fod.y + fod.height / 2
                readonly property real r: fod.width / 2
                Rectangle {
                    anchors.fill: parent
                    color: "black"
                    opacity: root.dimAlpha
                }
                Shape {
                    anchors.fill: parent
                    preferredRendererType: Shape.CurveRenderer
                    ShapePath {
                        strokeWidth: -1
                        fillGradient: RadialGradient {
                            centerX: press.cx; centerY: press.cy; centerRadius: press.r * 1.8
                            focalX: press.cx; focalY: press.cy; focalRadius: press.r * 1.15
                            GradientStop { position: 0; color: "white" }
                            GradientStop { position: 0.35; color: "#8cffffff" }
                            GradientStop { position: 1; color: "transparent" }
                        }
                        PathAngleArc {
                            centerX: press.cx; centerY: press.cy
                            radiusX: press.r * 1.8; radiusY: press.r * 1.8
                            startAngle: 0; sweepAngle: 360
                        }
                    }
                }
                // solid white over the sensor
                Rectangle {
                    x: press.cx - width / 2
                    y: press.cy - height / 2
                    width: press.r * 2.3
                    height: width
                    radius: width / 2
                    color: "white"
                }
            }

            // ---- always-on display: black, the clock box at a spot that
            // moves every minute (5% off the sides, 20% off top and bottom)
            Rectangle {
                id: aodView
                anchors.fill: parent
                color: "black"
                z: 10
                visible: opacity > 0
                opacity: ShellState.aodView ? 1 : 0
                Behavior on opacity {
                    NumberAnimation { duration: ShellState.aodView ? 350 : 0; easing.type: Easing.InQuad }
                }
                // swallow touches while it is up
                MouseArea { anchors.fill: parent; enabled: ShellState.aodView }

                Column {
                    id: aodBox
                    function along(size, box, margin, at) {
                        const lo = size * margin, hi = size * (1 - margin) - box
                        const pos = hi >= lo ? lo + at * (hi - lo) : (size - box) / 2
                        return Math.round(Math.max(0, Math.min(size - box, pos)))
                    }
                    x: along(aodView.width, width, 0.05, ShellState.aodSpot[0])
                    y: along(aodView.height, height, 0.20, ShellState.aodSpot[1])
                    opacity: ShellState.aodClock ? 1 : 0
                    Behavior on opacity {
                        NumberAnimation { duration: ShellState.aodClock ? 400 : 0; easing.type: Easing.OutQuad }
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: Qt.formatTime(ShellState.aodTime, "HH:mm")
                        color: "white"
                        font.family: Theme.font
                        font.pixelSize: 85
                        font.weight: Font.ExtraLight
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: Qt.formatDate(ShellState.aodTime, "ddd, d MMMM")
                        color: "#b0b0b0"
                        font.family: Theme.font
                        font.pixelSize: 20
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        topPadding: 8
                        visible: text !== ""
                        text: ShellState.aodBattery
                        color: "#808080"
                        font.family: Theme.font
                        font.pixelSize: 16
                    }
                }
            }
            Connections {
                target: aodView.Window.window
                function onFrameSwapped() {
                    ShellState.lockFrames++
                    root.frameOut()
                }
            }

            // ---- password; back to the clock page (and the fingerprint) with
            // the button above it or a swipe down above the keyboard
            MouseArea {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: prompt.top
                enabled: root.prompting
                property real startY: 0
                onPressed: mouse => startY = mouse.y
                onPositionChanged: mouse => { if (mouse.y - startY > 80) root.prompting = false }
            }
            Tap {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: prompt.top
                anchors.bottomMargin: 18
                width: backRow.width + 36
                height: 40
                visible: root.prompting
                baseColor: Theme.raised
                onClicked: root.prompting = false
                Row {
                    id: backRow
                    anchors.centerIn: parent
                    spacing: 8
                    SymIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        size: 16
                        name: root.fpUsable ? "auth-fingerprint-symbolic" : "go-down-symbolic"
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.fpUsable ? "Use fingerprint" : "Back"
                        color: Theme.fg
                        font.family: Theme.font
                        font.pixelSize: Theme.textSize
                    }
                }
            }
            PasswordPrompt {
                id: prompt
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: implicitHeight
                visible: root.prompting
                busy: root.busy
                message: root.message
                error: root.messageIsError
                digits: ShellState.keyboardDigits
                onVisibleChanged: if (visible) clear()
                onSubmitted: text => {
                    ShellState.keyboardDigits = digits
                    root.tryUnlock(text)
                    clear()
                }
                onTextChanged: idle.restart()
                onCancelled: root.prompting = false
            }
        }
    }
}
