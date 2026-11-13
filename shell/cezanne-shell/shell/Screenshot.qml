import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

// Screenshots and screen recordings, as GNOME's screenshot UI. Opened from
// the quick menu or Print: the screen is captured first (grim) and shown
// frozen; then a Selection (drag a rectangle, its corners resize it, a drag
// inside moves it; kept for the next time), the Screen or a Window (the
// active workspace's, picked by title: niri gives no window positions), and
// Screenshot or Screencast.
// Screenshots: the frozen frame, cut out with ffmpeg, a window through niri
// (its own content, also where something covers it); into
// ~/Pictures/Screenshots and the clipboard (wl-copy), with a notification.
// Screencasts: cezanne-screencast (niri's ScreenCast API, VA-API H.264 on
// cezanne-venc), ~/Videos/Screencasts; a red timer in the panel stops it.
Scope {
    id: shot
    readonly property string home: Quickshell.env("HOME")
    // (the sandbox: folders of its own)
    readonly property string pictures: Quickshell.env("CEZANNE_PICTURES") || home + "/Pictures/Screenshots"
    readonly property string videos: Quickshell.env("CEZANNE_VIDEOS") || home + "/Videos/Screencasts"
    readonly property string frozenPath: Quickshell.env("XDG_RUNTIME_DIR") + "/cezanne-screenshot.png"
    // (the sandbox: a recorder built there)
    readonly property string recorder: Quickshell.env("CEZANNE_SCREENCAST") || "/usr/lib/cezanne-shell/cezanne-screencast"
    readonly property bool wanted: ShellState.popup === "screenshot"

    property string mode: "selection"       // selection / screen / window
    property bool video: false
    property rect sel: Qt.rect(0, 0, 0, 0)  // logical px of the overlay
    property int windowId: -1
    property bool frozenReady: false
    property int frozenGen: 0
    readonly property var windows: Niri.windowList.filter(w => Niri.activeWorkspace && w.workspace_id === Niri.activeWorkspace.id)

    // ---- opening: menus away, the frame frozen, then the overlay
    function open() {
        if (ShellState.recording) { stopRecording(); return }
        ShellState.close()
        frozenReady = false
        openDelay.restart()
    }
    Timer { id: openDelay; interval: 250; onTriggered: grab.running = true }
    Process {
        id: grab
        command: ["grim", "-t", "png", "-l", "0", shot.frozenPath]
        onExited: code => {
            if (code !== 0) { shot.notify("Screenshot failed", "The screen could not be captured", ""); return }
            shot.frozenGen++
            shot.frozenReady = true
            const focused = shot.windows.find(w => w.id === Niri.focusedWindowId)
            shot.windowId = focused ? focused.id : (shot.windows.length ? shot.windows[0].id : -1)
            ShellState.popup = "screenshot"
        }
    }

    // ---- capture
    function stamp() { return Qt.formatDateTime(new Date(), "yyyy-MM-dd hh-mm-ss") }
    // the selection in buffer px (the frozen frame and the cast's buffers
    // are the output's physical size, in its current rotation)
    function cropPx() {
        const sx = frame.implicitWidth / ui.width, sy = frame.implicitHeight / ui.height
        const x = Math.round(sel.x * sx), y = Math.round(sel.y * sy)
        return [x, y, Math.round(sel.width * sx) & ~1, Math.round(sel.height * sy) & ~1]
    }
    function capture() {
        if (video) return startRecording()
        const out = pictures + "/Screenshot From " + stamp() + ".png"
        if (mode === "window") {
            if (windowId < 0) return
            ShellState.close()
            winShot.out = out
            winShot.command = ["niri", "msg", "action", "screenshot-window", "--id", String(windowId),
                               "--path", out]
            winShot.running = true
            return
        }
        const c = mode === "selection" ? cropPx() : ["", "", "", ""]
        save.out = out
        save.command = ["sh", "-c", 'mkdir -p "${1%/*}" && if [ -n "$4" ]; then ffmpeg -v error -y -i "$0" -vf "crop=$4:$5:$2:$3" "$1"; else cp "$0" "$1"; fi && wl-copy --type image/png < "$1"',
                        frozenPath, out].concat(c.map(String))
        save.running = true
        flash.restart()
    }
    Process {
        id: save
        property string out
        onExited: code => code === 0 ? shot.notifyShot(out) : shot.notify("Screenshot failed", "It could not be saved", "")
    }
    // niri writes the file and fills the clipboard itself; it may answer
    // before the file is there
    Process {
        id: winShot
        property string out
        onExited: code => {
            if (code !== 0) { shot.notify("Screenshot failed", "niri could not capture the window", ""); return }
            winWait.left = 20
            winWait.restart()
        }
    }
    Timer {
        id: winWait
        property int left: 20
        interval: 100
        onTriggered: winCheck.running = true
    }
    Process {
        id: winCheck
        command: ["test", "-s", winShot.out]
        onExited: code => {
            if (code === 0) shot.notifyShot(winShot.out)
            else if (--winWait.left > 0) winWait.restart()
        }
    }

    // ---- recording
    property string recSaved: ""
    property string recError: ""
    function startRecording() {
        const out = videos + "/Screencast From " + stamp() + ".mp4"
        let args = mode === "window" ? [out, "window", String(windowId)]
                 : [out, "monitor", ui.screen ? ui.screen.name : "DSI-1"]
        if (mode === "selection") args = args.concat(cropPx().map(String))
        recSaved = ""
        recError = ""
        rec.command = [recorder].concat(args)
        ShellState.close()
        recDelay.restart()                      // the overlay gone first
    }
    Timer { id: recDelay; interval: 300; onTriggered: rec.running = true }
    function stopRecording() { if (rec.running) rec.write("stop\n") }
    Process {
        id: rec
        stdinEnabled: true
        stdout: SplitParser {
            onRead: line => {
                if (line.startsWith("recording")) {
                    ShellState.recordingSince = Date.now()
                    ShellState.recording = true
                } else if (line.startsWith("saved ")) {
                    shot.recSaved = line.slice(6)
                } else if (line.startsWith("error ")) {
                    shot.recError = line.slice(6)
                }
            }
        }
        onExited: {
            ShellState.recording = false
            if (shot.recSaved !== "")
                shot.notify("Screencast recorded", "The recording has been saved in Videos / Screencasts", shot.recSaved, true)
            else
                shot.notify("Screencast failed", shot.recError || "The recorder stopped", "")
        }
    }

    // ---- notifications (to the shell's own server): open it, or show it
    // in Files
    function notifyShot(out) {
        notify("Screenshot captured", "You can paste the image from the clipboard.", out, true)
    }
    function notify(title, body, file, actions) {
        notifier.createObject(shot, {
            file: file,
            command: ["notify-send", "-a", file.endsWith(".mp4") ? "Screencast" : "Screenshot",
                      "-i", file.endsWith(".png") ? file : "applets-screenshooter-symbolic"]
                .concat(actions ? ["--action=default=Open", "--action=files=Show in Files"] : [])
                .concat([title, body])
        })
    }
    Component {
        id: notifier
        Process {
            id: np
            property string file
            running: true
            stdout: SplitParser {
                onRead: line => {
                    // started by niri (their own scope), not as the shell's children
                    if (line === "default") Niri.action(["spawn", "--", "xdg-open", np.file])
                    else if (line === "files") Niri.action(["spawn", "--", "nautilus", "--select", np.file])
                }
            }
            onExited: destroy()
        }
    }

    // ---- the overlay
    PanelWindow {
        id: ui
        screen: Quickshell.screens[0]
        anchors { top: true; bottom: true; left: true; right: true }
        exclusionMode: ExclusionMode.Ignore
        color: "black"
        visible: shot.wanted && shot.frozenReady
        WlrLayershell.namespace: "cezanne-screenshot"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: shot.wanted ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

        // the default selection (or the last one, if it still fits), once
        // the overlay has its size
        function fitSelection() {
            const s = shot.sel
            if (visible && width > 0 && height > 0 && (s.width < 40 || s.height < 40 ||
                                                         s.x + s.width > width || s.y + s.height > height))
                shot.sel = Qt.rect(width * 0.15, height * 0.25, width * 0.7, height * 0.35)
        }
        onVisibleChanged: fitSelection()
        onWidthChanged: fitSelection()
        onHeightChanged: fitSelection()

        Item {
            anchors.fill: parent
            focus: true
            Keys.onEscapePressed: ShellState.close()
            Keys.onReturnPressed: shot.capture()

            Image {
                id: frame
                anchors.fill: parent
                source: shot.frozenReady ? "file://" + shot.frozenPath + "?" + shot.frozenGen : ""
                cache: false
                smooth: true
            }

            // dim outside the selection; all over for a window (its title
            // chips pick it)
            readonly property color dim: "#99000000"
            Rectangle { visible: shot.mode === "window"; anchors.fill: parent; color: parent.dim }
            Item {
                anchors.fill: parent
                visible: shot.mode === "selection"
                Rectangle { x: 0; y: 0; width: parent.width; height: shot.sel.y; color: parent.parent.dim }
                Rectangle { x: 0; y: shot.sel.y + shot.sel.height; width: parent.width; height: parent.height - y; color: parent.parent.dim }
                Rectangle { x: 0; y: shot.sel.y; width: shot.sel.x; height: shot.sel.height; color: parent.parent.dim }
                Rectangle { x: shot.sel.x + shot.sel.width; y: shot.sel.y; width: parent.width - x; height: shot.sel.height; color: parent.parent.dim }
                Rectangle {
                    x: shot.sel.x - 1; y: shot.sel.y - 1
                    width: shot.sel.width + 2; height: shot.sel.height + 2
                    color: "transparent"
                    border.width: 2
                    border.color: "white"
                }
                Repeater {
                    model: 4
                    Rectangle {
                        required property int index
                        width: 18; height: 18; radius: 9
                        color: "white"
                        x: (index % 2 ? shot.sel.x + shot.sel.width : shot.sel.x) - 9
                        y: (index > 1 ? shot.sel.y + shot.sel.height : shot.sel.y) - 9
                    }
                }
            }
            // the whole screen: a frame around it
            Rectangle {
                visible: shot.mode === "screen"
                anchors.fill: parent
                color: "transparent"
                border.width: 4
                border.color: "white"
            }

            // the selection: a corner (within 28 px) resizes, inside moves,
            // elsewhere draws a new one
            MouseArea {
                anchors.fill: parent
                enabled: shot.mode === "selection"
                property string grip: ""
                property point start
                property rect from
                function near(px, py, cx, cy) { return Math.abs(px - cx) < 28 && Math.abs(py - cy) < 28 }
                onPressed: mouse => {
                    const s = shot.sel
                    start = Qt.point(mouse.x, mouse.y)
                    from = s
                    if (near(mouse.x, mouse.y, s.x, s.y)) grip = "tl"
                    else if (near(mouse.x, mouse.y, s.x + s.width, s.y)) grip = "tr"
                    else if (near(mouse.x, mouse.y, s.x, s.y + s.height)) grip = "bl"
                    else if (near(mouse.x, mouse.y, s.x + s.width, s.y + s.height)) grip = "br"
                    else if (mouse.x > s.x && mouse.x < s.x + s.width && mouse.y > s.y && mouse.y < s.y + s.height) grip = "move"
                    else grip = "new"
                }
                onPositionChanged: mouse => {
                    const W = width, H = height
                    const cl = (v, lo, hi) => Math.max(lo, Math.min(hi, v))
                    const dx = mouse.x - start.x, dy = mouse.y - start.y
                    let l = from.x, t = from.y, r = from.x + from.width, b = from.y + from.height
                    if (grip === "move") {
                        l = cl(from.x + dx, 0, W - from.width)
                        t = cl(from.y + dy, 0, H - from.height)
                        shot.sel = Qt.rect(l, t, from.width, from.height)
                        return
                    }
                    if (grip === "new") {
                        l = Math.min(start.x, mouse.x); r = Math.max(start.x, mouse.x)
                        t = Math.min(start.y, mouse.y); b = Math.max(start.y, mouse.y)
                    } else {
                        if (grip[1] === "l") l = from.x + dx; else r = from.x + from.width + dx
                        if (grip[0] === "t") t = from.y + dy; else b = from.y + from.height + dy
                    }
                    l = cl(l, 0, W); r = cl(r, 0, W); t = cl(t, 0, H); b = cl(b, 0, H)
                    if (r - l >= 16 && b - t >= 16)
                        shot.sel = Qt.rect(Math.min(l, r), Math.min(t, b), Math.abs(r - l), Math.abs(b - t))
                }
            }

            // the flash of a screenshot
            Rectangle {
                id: flashRect
                x: shot.mode === "selection" ? shot.sel.x : 0
                y: shot.mode === "selection" ? shot.sel.y : 0
                width: shot.mode === "selection" ? shot.sel.width : parent.width
                height: shot.mode === "selection" ? shot.sel.height : parent.height
                color: "white"
                opacity: 0
                SequentialAnimation {
                    id: flash
                    NumberAnimation { target: flashRect; property: "opacity"; from: 0.9; to: 0; duration: 220 }
                    ScriptAction { script: ShellState.close() }
                }
            }

            // ---- the toolbar
            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 28
                width: Math.min(parent.width - 24, 420)
                spacing: 10

                // a window, by title
                Flickable {
                    visible: shot.mode === "window"
                    width: parent.width
                    height: 40
                    contentWidth: chips.width
                    flickableDirection: Flickable.HorizontalFlick
                    clip: true
                    Row {
                        id: chips
                        spacing: 8
                        Repeater {
                            model: shot.windows
                            Tap {
                                required property var modelData
                                width: Math.min(chipText.implicitWidth + 32, 240)
                                height: 40
                                baseColor: shot.windowId === modelData.id ? Theme.accent : Theme.card
                                onClicked: shot.windowId = modelData.id
                                Text {
                                    id: chipText
                                    anchors.centerIn: parent
                                    width: parent.width - 24
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideRight
                                    text: modelData.title || modelData.app_id
                                    color: Theme.fg
                                    font.family: Theme.font
                                    font.pixelSize: Theme.textSize
                                }
                            }
                        }
                    }
                }
                Text {
                    visible: shot.mode === "window" && shot.windows.length === 0
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: "No windows on this workspace"
                    color: Theme.fg
                    font.family: Theme.font
                    font.pixelSize: Theme.textSize
                }

                Rectangle {
                    width: parent.width
                    height: bar.implicitHeight + 24
                    radius: Theme.radius
                    color: Theme.card
                    Column {
                        id: bar
                        anchors.centerIn: parent
                        width: parent.width - 24
                        spacing: 12

                        // Selection / Screen / Window
                        Row {
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: 8
                            Repeater {
                                model: [["selection", "Selection", "icons/screenshot-area-symbolic.svg"],
                                        ["screen", "Screen", "video-display-symbolic"],
                                        ["window", "Window", "focus-windows-symbolic"]]
                                Tap {
                                    required property var modelData
                                    width: (bar.width - 16) / 3
                                    height: 52
                                    radius: 16
                                    baseColor: shot.mode === modelData[0] ? Theme.raised : "transparent"
                                    onClicked: shot.mode = modelData[0]
                                    Column {
                                        anchors.centerIn: parent
                                        spacing: 3
                                        SymIcon {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            size: 18
                                            name: modelData[2]
                                        }
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: modelData[1]
                                            color: Theme.fg
                                            font.family: Theme.font
                                            font.pixelSize: Theme.smallText
                                        }
                                    }
                                }
                            }
                        }

                        // Screenshot | Screencast, the capture button, close
                        Item {
                            width: parent.width
                            height: 72
                            Row {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 0
                                Repeater {
                                    model: [[false, "camera-photo-symbolic"], [true, "camera-video-symbolic"]]
                                    Tap {
                                        required property var modelData
                                        required property int index
                                        width: 48; height: 40
                                        radius: 20
                                        baseColor: shot.video === modelData[0] ? Theme.raised : "transparent"
                                        onClicked: shot.video = modelData[0]
                                        SymIcon { anchors.centerIn: parent; size: 18; name: modelData[1] }
                                    }
                                }
                            }
                            MouseArea {
                                id: captureButton
                                anchors.centerIn: parent
                                width: 64; height: 64
                                enabled: shot.mode !== "window" || shot.windowId >= 0
                                onClicked: shot.capture()
                                Rectangle {
                                    anchors.fill: parent
                                    radius: width / 2
                                    color: "transparent"
                                    border.width: 4
                                    border.color: "white"
                                    opacity: captureButton.enabled ? 1 : 0.4
                                }
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: captureButton.pressed ? 44 : 50
                                    height: width
                                    radius: width / 2
                                    color: shot.video ? "#e01b24" : "white"
                                    opacity: captureButton.enabled ? 1 : 0.4
                                    Behavior on width { NumberAnimation { duration: 80 } }
                                }
                            }
                            Tap {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                width: 40; height: 40
                                baseColor: Theme.raised
                                onClicked: ShellState.close()
                                SymIcon { anchors.centerIn: parent; size: 16; name: "window-close-symbolic" }
                            }
                        }
                    }
                }
            }
        }
    }
}
