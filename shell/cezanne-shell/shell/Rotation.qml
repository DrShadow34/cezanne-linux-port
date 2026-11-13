import QtQuick
import Quickshell
import Quickshell.Io

// Auto-rotation: iio-sensor-proxy's accelerometer orientation (through
// monitor-sensor) turned into niri's output transform, unless locked in the
// quick menu. The panel is portrait, so "normal" is portrait. Locked, the
// panel is portrait whatever the sensor says: the fingerprint spot only
// lines up with the sensor unrotated (as cezanne-fod's lockPortrait did).
Scope {
    id: root
    property string orientation: "normal"
    readonly property string output: Quickshell.screens.length > 0 ? Quickshell.screens[0].name : ""
    property string applied: ""
    property string beforeLock: ""

    // wl_output transforms are counter-clockwise, as mutter maps them
    readonly property var transforms: ({ "normal": "normal", "left-up": "90", "right-up": "270", "bottom-up": "180" })

    function set(t) {
        if (!t || t === applied || output === "") return
        applied = t
        ShellState.transform = t
        Niri.setTransform(output, t)
    }
    function apply() {
        if (ShellState.rotationLock || ShellState.locked) return
        set(transforms[orientation])
    }

    onOrientationChanged: apply()
    Connections {
        target: ShellState
        function onRotationLockChanged() { root.apply() }
        // unlocked: the sensor's orientation, or with the rotation lock on
        // the one before the lock
        function onLockedChanged() {
            if (ShellState.locked) {
                root.beforeLock = root.applied
                root.set("normal")
            } else if (ShellState.rotationLock) {
                root.set(root.beforeLock)
            } else {
                root.apply()
            }
        }
    }

    Process {
        id: sensor
        running: true
        command: ["monitor-sensor", "--accel"]
        stdout: SplitParser {
            onRead: line => {
                // "=== Has accelerometer (orientation: normal)" and
                // "    Accelerometer orientation changed: left-up"
                const m = line.match(/orientation(?: changed)?: ([a-z-]+)/)
                if (m && m[1] !== "undefined") root.orientation = m[1]
            }
        }
        onExited: restart.start()
    }
    Timer {
        id: restart
        interval: 5000
        onTriggered: sensor.running = true
    }
}
