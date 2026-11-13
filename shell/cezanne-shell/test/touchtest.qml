import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

// Touch calibration check (run in the session: qs -p touchtest.qml): three
// crosses at known logical positions; for each tap it logs where niri
// delivered the touch (Qt, "QT") and where cezanne-touch puts it
// ("HELPER", mapped like Touch.qml with the normal transform). Quits after
// three taps or three minutes; read the log with journalctl.
ShellRoot {
    id: root
    property int taps: 0
    readonly property var targets: [[270, 300], [100, 700], [440, 1000]]

    PanelWindow {
        id: win
        anchors { top: true; bottom: true; left: true; right: true }
        exclusionMode: ExclusionMode.Ignore
        color: "#e6101018"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.namespace: "cezanne-touchtest"

        Text {
            y: 120
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Tap the centre of each red cross (" + root.taps + "/3)"
            color: "white"
            font.pixelSize: 18
        }
        Repeater {
            model: root.targets
            Item {
                required property var modelData
                x: modelData[0]
                y: modelData[1]
                Rectangle { x: -24; y: -1; width: 48; height: 2; color: "#ff4040" }
                Rectangle { x: -1; y: -24; width: 2; height: 48; color: "#ff4040" }
            }
        }
        MouseArea {
            anchors.fill: parent
            onPressed: mouse => console.info("QT down", win.width, win.height, mouse.x.toFixed(1), mouse.y.toFixed(1))
            onReleased: mouse => {
                console.info("QT up", mouse.x.toFixed(1), mouse.y.toFixed(1))
                root.taps++
                if (root.taps >= 3) Qt.quit()
            }
        }
    }

    Process {
        running: true
        stdinEnabled: true
        command: ["/usr/lib/cezanne-shell/cezanne-touch"]
        onStarted: write("taps 1\n")
        stdout: SplitParser {
            onRead: line => {
                const f = line.split(" ")
                if (f[0] === "d" || f[0] === "m")
                    console.info("HELPER", f[0], f[1], (parseFloat(f[2]) * win.width).toFixed(1), (parseFloat(f[3]) * win.height).toFixed(1))
                else if (f[0] === "u")
                    console.info("HELPER u", f[1])
            }
        }
    }
    Timer { running: true; interval: 180000; onTriggered: Qt.quit() }
}
