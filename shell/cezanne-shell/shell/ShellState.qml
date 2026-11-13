pragma Singleton
import QtQuick
import Quickshell
import Quickshell.Io

// Shared state: which popup is open, the lock, and the settings kept across
// restarts (~/.local/state/quickshell/.../state.json)
Singleton {
    id: root

    // one popup at a time: "", "quick", "calendar", "power", "launcher"
    property string popup: ""
    // the quick menu's list open when it opens: "", "wifi", "bt"
    property string quickSection: ""
    // text for the launcher's search field (IPC `launcher search`: the
    // sandbox has no touch to focus the field)
    property string launcherQuery: ""
    function toggle(name) { quickSection = ""; popup = popup === name ? "" : name }
    function close() { popup = ""; quickSection = "" }

    property bool locked: false
    // the lock surface is up (niri confirmed the lock)
    property bool lockSecure: false
    // bumped for every frame the lock surface puts out (AOD waits for them)
    property int lockFrames: 0
    // always-on display: the view on the lock surface (Aod.qml drives it)
    property bool aodView: false
    property bool aodClock: false
    property date aodTime: new Date()
    property var aodSpot: [0.5, 0.5]
    property string aodBattery: ""
    property var screenshotControl: null  // Screenshot.qml (open, stopRecording)
    property bool recording: false        // a screencast runs (the panel's timer)
    property real recordingSince: 0
    property var usbControl: null       // Usb.qml (the "Use USB for" dialog's state)
    property var nfcControl: null       // Nfc.qml (enabled, setEnabled, subtitle)
    property var aodControl: null       // Aod.qml (enabled, setEnabled)
    // the power menu's Suspend: into the AOD when it is on
    signal sleepRequested()
    // the lock survives a restart of the shell: niri stays locked when its
    // lock client goes away, and only a client that locks again can unlock
    // it. A runtime file (gone at logout) says to lock at once on start
    // (CEZANNE_LOCK_FILE: the sandbox's own, not the session's).
    onLockedChanged: lockFile.setText(locked ? "1\n" : "0\n")
    FileView {
        id: lockFile
        path: Quickshell.env("CEZANNE_LOCK_FILE") || Quickshell.env("XDG_RUNTIME_DIR") + "/cezanne-shell-locked"
        printErrors: false
        atomicWrites: false
        onLoaded: if (text().trim() === "1") root.locked = true
    }
    // the fade to black before the lock screen comes up
    property bool fading: false
    function lock() {
        if (locked || fading) return
        close()
        fading = true
        fadeTimer.restart()
    }
    Timer {
        id: fadeTimer
        interval: 280
        onTriggered: { root.locked = true; root.fading = false }
    }
    // a fullscreen window (game, video): panel and dock step aside
    property bool fullscreen: false
    // the dock, revealed from the bottom edge while windows cover it
    property bool dockRevealed: false

    property alias rotationLock: saved.rotationLock
    property alias dnd: saved.dnd
    // no idle blank / lock / sleep (the power key still sleeps)
    property alias caffeine: saved.caffeine
    // a spot under every finger (cezanne-show-taps did it in GNOME)
    property alias showTaps: saved.showTaps
    property alias flashLevel: saved.flashLevel
    property alias autoBias: saved.autoBias
    // the panel's output transform now (Rotation sets it; Touch maps with it)
    property string transform: "normal"
    property alias pinned: saved.pinned
    property alias keyboardDigits: saved.keyboardDigits

    function isPinned(id) { return pinned.indexOf(id) >= 0 }
    function togglePin(id) {
        const p = pinned.slice()
        const i = p.indexOf(id)
        if (i >= 0) p.splice(i, 1); else p.push(id)
        pinned = p
    }

    FileView {
        path: Quickshell.statePath("state.json")
        onAdapterUpdated: writeAdapter()
        onLoadFailed: error => { if (error === FileViewError.FileNotFound) writeAdapter() }

        JsonAdapter {
            id: saved
            property bool rotationLock: false
            property bool dnd: false
            property bool caffeine: false
            property bool showTaps: true
            property real flashLevel: 0.5
            // automatic brightness: the slider as a gain, 0.5 = the curve
            property real autoBias: 0.5
            property bool keyboardDigits: false
            // GNOME's favourites at the switch
            property var pinned: ["org.gnome.Nautilus", "org.gnome.Settings", "org.gnome.Snapshot",
                                  "firefox", "io.missioncenter.MissionCenter"]
        }
    }
}
