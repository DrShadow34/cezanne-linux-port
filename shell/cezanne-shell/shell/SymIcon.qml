import QtQuick
import QtQuick.Effects
import Quickshell

// A symbolic icon from the icon theme in one colour (Qt does not recolour
// symbolic icons the way GTK does)
Item {
    id: root
    property string name
    property color color: Theme.fg
    property int size: 16
    implicitWidth: size
    implicitHeight: size

    Image {
        id: img
        anchors.fill: parent
        // names with a slash: the shell's own icons (icons/NAME.svg)
        source: root.name === "" ? ""
              : root.name.indexOf("/") >= 0 ? Quickshell.shellPath(root.name)
              : Quickshell.iconPath(root.name, true)
        sourceSize: Qt.size(root.size, root.size)
        visible: false
    }
    MultiEffect {
        source: img
        anchors.fill: img
        brightness: 1.0
        colorization: 1.0
        colorizationColor: root.color
    }
}
