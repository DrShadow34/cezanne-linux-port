import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

// cezanne-shell: the phone shell around niri. Panel, quick menu, calendar
// and notifications, launcher, dock, lock screen, polkit agent, rotation,
// volume OSD. IPC (cezanne-shell-ctl): launcher, popup, lock.
ShellRoot {
    id: root
    // no live reload when the package replaces the QML: the reload lets go
    // of the session lock for a moment (niri: "unlocking session", 35 ms,
    // then locked again); a new version comes with the next shell start
    settings.watchFiles: false

    Notifications { id: notificationCenter }
    NotificationLight { notifications: notificationCenter }

    Variants {
        model: Quickshell.screens
        Wallpaper {}
    }
    Variants {
        model: Quickshell.screens
        Panel { notifications: notificationCenter }
    }
    Variants {
        model: Quickshell.screens
        Popups { notifications: notificationCenter }
    }
    Variants {
        model: Quickshell.screens
        Launcher {}
    }
    Variants {
        model: Quickshell.screens
        Dock {}
    }

    LockScreen { id: lockScreen; notifications: notificationCenter }
    Aod {
        id: aodControl
        Component.onCompleted: ShellState.aodControl = aodControl
    }
    Nfc {
        id: nfcControl
        Component.onCompleted: ShellState.nfcControl = nfcControl
    }
    Usb {
        id: usbControl
        Component.onCompleted: ShellState.usbControl = usbControl
    }
    Screenshot {
        id: screenshotControl
        Component.onCompleted: ShellState.screenshotControl = screenshotControl
    }
    Rotation {}
    Touch {}
    PolkitDialog {}
    Osd {}

    // a fullscreen window (game, video): no panel or dock, no banners, and
    // no idle blank / lock / sleep (cezanne-game-awake did this in GNOME)
    Binding {
        target: ShellState
        property: "fullscreen"
        value: ToplevelManager.toplevels.values.some(t => t.fullscreen && t.activated)
    }
    PanelWindow {
        id: inhibitorSurface
        screen: Quickshell.screens[0]
        anchors { top: true; left: true }
        implicitWidth: 1
        implicitHeight: 1
        exclusionMode: ExclusionMode.Ignore
        color: "transparent"
        mask: Region {}
        WlrLayershell.namespace: "cezanne-inhibitor"
        WlrLayershell.layer: WlrLayer.Overlay
        IdleInhibitor {
            window: inhibitorSurface
            // (and while the screen streams as a USB webcam: a lock would end it)
            enabled: ShellState.caffeine || (ShellState.fullscreen && !ShellState.locked) || usbControl.screenStream
                     || ShellState.recording
        }
    }

    // the fade to black before locking (and so before sleep)
    PanelWindow {
        screen: Quickshell.screens[0]
        anchors { top: true; bottom: true; left: true; right: true }
        exclusionMode: ExclusionMode.Ignore
        color: "transparent"
        visible: ShellState.fading || fadeIn.running
        mask: Region {}
        WlrLayershell.namespace: "cezanne-fade"
        WlrLayershell.layer: WlrLayer.Overlay
        Rectangle {
            anchors.fill: parent
            color: "black"
            opacity: ShellState.fading ? 1 : 0
            Behavior on opacity { NumberAnimation { id: fadeIn; duration: 260; easing.type: Easing.InQuad } }
        }
    }

    // an app taking the focus (opened, or picked in the overview) closes the
    // menus; a menu's own keyboard focus moves niri's window focus to none,
    // which must not (it closed every menu over an open app), and neither
    // must the focus going back to the window that had it when the menus
    // opened (from the launcher to the quick menu, the launcher's exclusive
    // focus ends before the menu's begins)
    Connections {
        target: Niri
        property var before: null
        function onFocusedWindowIdChanged() {
            const id = Niri.focusedWindowId
            if (ShellState.popup === "") before = id
            else if (id !== null && id !== before && ShellState.popup !== "launcher") ShellState.close()
        }
    }

    IpcHandler {
        target: "launcher"
        function toggle(): void { Niri.closeOverview(); ShellState.toggle("launcher") }
        function open(): void { ShellState.popup = "launcher" }
        function close(): void { ShellState.close() }
    }
    IpcHandler {
        target: "popup"
        function toggle(name: string): void { ShellState.toggle(name) }
        // the quick menu with its Wi-Fi ("wifi") or Bluetooth ("bt") list open
        function quick(section: string): void { ShellState.popup = "quick"; ShellState.quickSection = section }
        function close(): void { ShellState.close() }
    }
    IpcHandler {
        target: "aod"
        // niri's XF86PowerOff bind (logind leaves the key to the shell
        // while the AOD is on)
        function power(): void { aodControl.powerKey() }
        // swayidle: idle for 60 s
        function idle(): void { aodControl.idle() }
    }
    IpcHandler {
        target: "screenshot"
        // the screenshot UI (Print); while recording it stops the recording
        function open(): void { screenshotControl.open() }
        function stop(): void { screenshotControl.stopRecording() }
    }
    IpcHandler {
        target: "usb"
        // the dialog (page: modes / sources); a mode as the dialog sets it
        function dialog(page: string): void { usbControl.openDialog(page === "sources" ? "sources" : "modes") }
        function setMode(mode: string, source: string): void { usbControl.setMode(mode, source) }
    }
    IpcHandler {
        target: "caffeine"
        // standby measurements turn it off for the run and back after
        function get(): bool { return ShellState.caffeine }
        function set(on: bool): void { ShellState.caffeine = on }
    }
    IpcHandler {
        target: "lock"
        function lock(): void { ShellState.lock() }
        // true once niri shows the lock surface (swayidle waits for it
        // before the system sleeps)
        function isLocked(): bool { return lockScreen.secure }
        function prompt(): void { lockScreen.showPrompt() }
    }
}
