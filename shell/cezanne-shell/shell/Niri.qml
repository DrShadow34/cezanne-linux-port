pragma Singleton
import QtQuick
import Quickshell
import Quickshell.Io

// niri's state from its IPC event stream ($NIRI_SOCKET), actions through
// `niri msg action` (one request per connection)
Singleton {
    id: niri

    readonly property string socketPath: Quickshell.env("NIRI_SOCKET") ?? ""
    property var windows: ({})       // id -> niri window
    property var workspaces: ({})    // id -> niri workspace
    property bool overview: false
    property var focusedWindowId: null

    readonly property var allWindows: Object.values(windows).sort((a, b) => a.id - b.id)
    // apps: everything but the home column (cezanne-home)
    readonly property var windowList: allWindows.filter(w => w.app_id !== "cezanne-home")
    readonly property var home: allWindows.find(w => w.app_id === "cezanne-home") ?? null
    readonly property bool homeFocused: home !== null && focusedWindowId === home.id
    readonly property var workspaceList: Object.values(workspaces).sort((a, b) => a.idx - b.idx)
    readonly property var activeWorkspace: workspaceList.find(w => w.is_active) ?? null
    readonly property int windowsOnActive: activeWorkspace
        ? windowList.filter(w => w.workspace_id === activeWorkspace.id).length : 0

    function windowsOf(appId) { return windowList.filter(w => w.app_id === appId) }

    function action(args) { Quickshell.execDetached(["niri", "msg", "action"].concat(args)) }
    function toggleOverview() { action(["toggle-overview"]) }
    function closeOverview() { if (overview) action(["close-overview"]) }
    function focusWindow(id) { action(["focus-window", "--id", String(id)]) }
    function closeWindow(id) { action(["close-window", "--id", String(id)]) }
    function spawnSh(cmd) { action(["spawn-sh", "--", cmd]) }
    function setTransform(output, t) { Quickshell.execDetached(["niri", "msg", "output", output, "transform", t]) }
    function monitorsOff() { action(["power-off-monitors"]) }

    function handle(line) {
        let ev
        try { ev = JSON.parse(line) } catch (e) { return }
        if (typeof ev !== "object" || "Ok" in ev || "Err" in ev) return
        const kind = Object.keys(ev)[0]
        const v = ev[kind]
        switch (kind) {
        case "WorkspacesChanged": {
            const m = {}
            for (const w of v.workspaces) m[w.id] = w
            workspaces = m
            break
        }
        case "WorkspaceActivated": {
            const m = Object.assign({}, workspaces)
            const out = m[v.id] ? m[v.id].output : null
            for (const id in m) {
                if (m[id].output === out) m[id] = Object.assign({}, m[id], { is_active: m[id].id === v.id })
                if (v.focused) m[id] = Object.assign({}, m[id], { is_focused: m[id].id === v.id })
            }
            workspaces = m
            break
        }
        case "WorkspaceActiveWindowChanged": {
            if (!workspaces[v.workspace_id]) break
            const m = Object.assign({}, workspaces)
            m[v.workspace_id] = Object.assign({}, m[v.workspace_id], { active_window_id: v.active_window_id })
            workspaces = m
            break
        }
        case "WindowsChanged": {
            const m = {}
            for (const w of v.windows) {
                m[w.id] = w
                if (w.is_focused) focusedWindowId = w.id
            }
            windows = m
            break
        }
        case "WindowOpenedOrChanged": {
            const m = Object.assign({}, windows)
            if (v.window.is_focused) {
                for (const id in m) if (m[id].is_focused) m[id] = Object.assign({}, m[id], { is_focused: false })
                focusedWindowId = v.window.id
            }
            m[v.window.id] = v.window
            windows = m
            break
        }
        case "WindowClosed": {
            const m = Object.assign({}, windows)
            delete m[v.id]
            windows = m
            if (focusedWindowId === v.id) focusedWindowId = null
            break
        }
        case "WindowFocusChanged": {
            const m = Object.assign({}, windows)
            for (const id in m) {
                const f = m[id].id === v.id
                if (m[id].is_focused !== f) m[id] = Object.assign({}, m[id], { is_focused: f })
            }
            windows = m
            focusedWindowId = v.id
            break
        }
        case "OverviewOpenedOrClosed":
            overview = v.is_open
            break
        }
    }

    Socket {
        id: events
        path: niri.socketPath
        connected: niri.socketPath !== ""
        onConnectedChanged: {
            if (connected) {
                write('"EventStream"\n')
                flush()
            } else {
                retry.start()
            }
        }
        parser: SplitParser {
            onRead: line => niri.handle(line)
        }
    }

    // niri restarted or not up yet
    Timer {
        id: retry
        interval: 1000
        onTriggered: if (!events.connected && niri.socketPath !== "") events.connected = true
    }
}
