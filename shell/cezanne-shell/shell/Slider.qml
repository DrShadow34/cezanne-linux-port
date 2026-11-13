import QtQuick

// A GNOME-style slider row: icon (tappable), then the track
Item {
    id: root
    property real value: 0          // 0..1
    property string icon
    property bool iconActive: true
    signal moved(real value)
    signal iconClicked()
    implicitHeight: 44

    Tap {
        id: iconTap
        width: 40; height: 40
        anchors.verticalCenter: parent.verticalCenter
        onClicked: root.iconClicked()
        SymIcon {
            anchors.centerIn: parent
            size: 18
            name: root.icon
            color: root.iconActive ? Theme.fg : Theme.fgFaint
        }
    }

    Item {
        id: track
        anchors.left: iconTap.right
        anchors.leftMargin: 8
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        height: 36

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 6
            radius: 3
            color: Theme.raised
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(6, root.value * parent.width)
            height: 6
            radius: 3
            color: Theme.accent
        }
        Rectangle {
            x: root.value * (parent.width - width)
            anchors.verticalCenter: parent.verticalCenter
            width: 22; height: 22; radius: 11
            color: Theme.fg
        }
        MouseArea {
            anchors.fill: parent
            anchors.margins: -6
            preventStealing: true
            function set(mx) {
                const v = Math.min(1, Math.max(0, (mx - 6) / (track.width)))
                root.value = v
                root.moved(v)
            }
            onPressed: mouse => set(mouse.x)
            onPositionChanged: mouse => set(mouse.x)
        }
    }
}
