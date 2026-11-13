import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

// Touch beyond what niri does itself: three-finger swipes (cezanne-touch
// reads the touchscreen beside niri), "show taps" spots, and "back" by a
// swipe in from the left or right edge.
//   3 fingers up: overview, again: app grid (a menu open: close it);
//   down: the quick menu (the overview open: close it);
//   left / right: the next / previous app (niri's columns); past the last
//   one the empty workspace (home), and back from there.
//   back: hide the on-screen keyboard, close a menu, the launcher or the
//   overview, else Alt+Left to the app (back in Firefox, Files, Settings,
//   libadwaita pages).
Scope {
    id: root
    readonly property var screen: Quickshell.screens[0]
    property var fingers: ({})      // slot -> [u, v] (panel-native 0..1)
    readonly property bool touching: Object.keys(fingers).length > 0
    property int fading: 0          // spots still fading out after a lift

    // panel-native (portrait) 0..1 -> the rotated screen's logical pixels
    function toScreen(u, v, w, h) {
        switch (ShellState.transform) {
        case "90": return [w * (1 - v), h * u]
        case "270": return [w * v, h * (1 - u)]
        case "180": return [w * (1 - u), h * (1 - v)]
        default: return [w * u, h * v]
        }
    }
    function toScreenVector(du, dv) {
        switch (ShellState.transform) {
        case "90": return [-dv, du]
        case "270": return [dv, -du]
        case "180": return [-du, -dv]
        default: return [du, dv]
        }
    }

    function swipe(du, dv) {
        if (ShellState.locked) return
        const [dx, dy] = toScreenVector(du, dv)
        const ax = Math.abs(dx), ay = Math.abs(dy)
        if (Math.max(ax, ay) < 0.1) return
        if (ay > ax * 1.3) {
            if (dy < 0) {
                if (ShellState.popup !== "") ShellState.close()
                else if (Niri.overview) { Niri.closeOverview(); ShellState.popup = "launcher" }
                else Niri.toggleOverview()
            } else {
                if (Niri.overview) Niri.closeOverview()
                else if (ShellState.popup !== "quick") ShellState.popup = "quick"
            }
        } else if (ax > ay * 1.3) {
            ShellState.close()
            Niri.closeOverview()
            sideways(dx < 0)
        }
    }

    // to the next app (toward newer columns) or the previous one; past
    // either end the home column (cezanne-home: empty, the wallpaper and the
    // dock), within the workspace
    function sideways(next) {
        const ws = Niri.activeWorkspace
        if (!ws) return
        const home = Niri.home && Niri.home.workspace_id === ws.id ? Niri.home : null
        const tiled = Niri.allWindows.filter(w => w.workspace_id === ws.id && !w.is_floating)
        const colOf = w => w?.layout?.pos_in_scrolling_layout?.[0] ?? 1
        const focused = Niri.windows[Niri.focusedWindowId] ?? Niri.windows[ws.active_window_id]
        const col = colOf(focused)
        const cols = tiled.map(colOf)
        const first = Math.min(...cols), last = Math.max(...cols)
        if (focused && home && focused.id === home.id) {
            // from home: into the apps, either way
            if (tiled.length > 1) Niri.action([next ? "focus-column-right" : "focus-column-last"])
            return
        }
        if (tiled.length > 0 && (next ? col < last : col > first)) {
            // the home column sits at the start: stepping back onto it is
            // also the way past the first app
            Niri.action([next ? "focus-column-right" : "focus-column-left"])
            return
        }
        if (home) Niri.focusWindow(home.id)
    }

    function back() {
        oskBack.running = true
    }
    function backPastKeyboard() {
        if (ShellState.popup !== "") ShellState.close()
        else if (Niri.overview) Niri.closeOverview()
        else Quickshell.execDetached(["wtype", "-M", "alt", "-k", "Left", "-m", "alt"])
    }
    // back first hides squeekboard when it is up (it has no hide key)
    Process {
        id: oskBack
        command: ["sh", "-c", "[ \"$(busctl --user get-property sm.puri.OSK0 /sm/puri/OSK0 sm.puri.OSK0 Visible 2>/dev/null)\" = 'b true' ] && busctl --user call sm.puri.OSK0 /sm/puri/OSK0 sm.puri.OSK0 SetVisible b false && echo hidden"]
        stdout: StdioCollector {
            onStreamFinished: if (text.trim() !== "hidden") root.backPastKeyboard()
        }
    }

    Process {
        id: helper
        running: true
        stdinEnabled: true
        command: ["/usr/lib/cezanne-shell/cezanne-touch"]
        onStarted: write(ShellState.showTaps ? "taps 1\n" : "taps 0\n")
        onExited: restartHelper.start()
        stdout: SplitParser {
            onRead: line => {
                const f = line.split(" ")
                if (f[0] === "g3") { root.swipe(parseFloat(f[1]), parseFloat(f[2])); return }
                const m = Object.assign({}, root.fingers)
                if (f[0] === "d" || f[0] === "m") m[f[1]] = [parseFloat(f[2]), parseFloat(f[3])]
                else if (f[0] === "u") {
                    // the lifted finger's spot fades on its own, where it was
                    const at = m[f[1]]
                    if (at && taps.visible) {
                        const p = root.toScreen(at[0], at[1], taps.width, taps.height)
                        fadeSpot.createObject(taps.contentItem, { x: p[0] - 22, y: p[1] - 22 })
                        root.fading++
                    }
                    delete m[f[1]]
                } else return
                root.fingers = m
            }
        }
    }
    Timer { id: restartHelper; interval: 3000; onTriggered: helper.running = true }
    Connections {
        target: ShellState
        function onShowTapsChanged() {
            if (helper.running) helper.write(ShellState.showTaps ? "taps 1\n" : "taps 0\n")
            if (!ShellState.showTaps) root.fingers = {}
        }
    }

    // ---- show taps: a spot under every finger, on top of everything; the
    // layer is only up while fingers are down or spots fade (else a
    // fullscreen game could not go straight to the display). A lifted
    // finger's spot is handed to a separate fading copy, and the layer stays
    // up until every copy is gone: when one spot faded in place, the layer
    // could unmap on a half-faded frame and show it again on the next touch
    // (a ghost beside the finger).
    Component {
        id: fadeSpot
        Rectangle {
            id: spot
            width: 44; height: 44; radius: 22
            color: "#66ffffff"
            border.width: 2
            border.color: "#b3000000"
            NumberAnimation on opacity {
                from: 1; to: 0
                duration: 280
                easing.type: Easing.OutQuad
                onFinished: { root.fading--; spot.destroy() }
            }
        }
    }
    PanelWindow {
        id: taps
        screen: root.screen
        anchors { top: true; bottom: true; left: true; right: true }
        exclusionMode: ExclusionMode.Ignore
        color: "transparent"
        visible: ShellState.showTaps && (root.touching || root.fading > 0 || linger.running)
        mask: Region {}
        WlrLayershell.namespace: "cezanne-taps"
        WlrLayershell.layer: WlrLayer.Overlay

        // one more frame without spots before the layer unmaps
        Timer { id: linger; interval: 120; running: !root.touching && root.fading === 0 }

        Repeater {
            model: 10
            Rectangle {
                required property int index
                readonly property var at: root.fingers[index] ?? null
                readonly property var pos: at ? root.toScreen(at[0], at[1], taps.width, taps.height) : [0, 0]
                visible: at !== null
                x: pos[0] - width / 2
                y: pos[1] - height / 2
                width: 44; height: 44; radius: 22
                color: "#66ffffff"
                border.width: 2
                border.color: "#b3000000"
            }
        }
    }

    // ---- back: a swipe in from either edge
    component EdgeStrip: PanelWindow {
        id: strip
        required property bool left
        screen: root.screen
        anchors { top: true; bottom: true; left: strip.left; right: !strip.left }
        implicitWidth: 10
        exclusiveZone: 0
        color: "transparent"
        visible: !ShellState.locked && !ShellState.fullscreen
        WlrLayershell.namespace: "cezanne-back-edge"
        WlrLayershell.layer: WlrLayer.Top

        MouseArea {
            anchors.fill: parent
            property real startX: 0
            property bool fired: false
            onPressed: mouse => { startX = mouse.x; fired = false }
            onPositionChanged: mouse => {
                const inward = strip.left ? mouse.x - startX : startX - mouse.x
                if (!fired && inward > 48) { fired = true; root.back() }
            }
        }
    }
    EdgeStrip { left: true }
    EdgeStrip { left: false }
}
