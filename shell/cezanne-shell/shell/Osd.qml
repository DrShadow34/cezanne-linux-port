import QtQuick
import Quickshell
import Quickshell.Wayland
import Quickshell.Services.Pipewire

// The volume level for a moment when it changes (volume keys)
Scope {
    id: root
    readonly property var sink: Pipewire.defaultAudioSink
    property bool showing: false
    property bool ready: false

    PwObjectTracker { objects: [root.sink] }

    // the first values come in at startup: no OSD for them
    Timer { running: true; interval: 2000; onTriggered: root.ready = true }
    Timer { id: hide; interval: 1500; onTriggered: root.showing = false }

    Connections {
        target: root.sink?.audio ?? null
        function onVolumesChanged() { root.pop() }
        function onMutedChanged() { root.pop() }
    }
    function pop() {
        if (!ready || ShellState.popup === "quick") return
        showing = true
        hide.restart()
    }

    PanelWindow {
        screen: Quickshell.screens[0]
        anchors.bottom: true
        margins.bottom: 120
        implicitWidth: 260
        implicitHeight: 52
        exclusionMode: ExclusionMode.Ignore
        color: "transparent"
        visible: root.showing
        WlrLayershell.namespace: "cezanne-osd"
        WlrLayershell.layer: WlrLayer.Overlay
        mask: Region {}

        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: Theme.card
            readonly property real vol: root.sink ? Math.min(1, root.sink.audio.volume) : 0
            readonly property bool muted: root.sink?.audio.muted ?? true
            SymIcon {
                id: ic
                anchors.left: parent.left
                anchors.leftMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                size: 18
                name: parent.muted || parent.vol === 0 ? "audio-volume-muted-symbolic"
                    : parent.vol > 0.66 ? "audio-volume-high-symbolic"
                    : parent.vol > 0.33 ? "audio-volume-medium-symbolic" : "audio-volume-low-symbolic"
            }
            Rectangle {
                anchors.left: ic.right
                anchors.leftMargin: 14
                anchors.right: parent.right
                anchors.rightMargin: 22
                anchors.verticalCenter: parent.verticalCenter
                height: 6; radius: 3
                color: Theme.raised
                Rectangle {
                    width: parent.width * (parent.parent.muted ? 0 : parent.parent.vol)
                    height: parent.height
                    radius: 3
                    color: Theme.fg
                }
            }
        }
    }
}
