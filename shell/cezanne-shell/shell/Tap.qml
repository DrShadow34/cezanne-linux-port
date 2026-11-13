import QtQuick

// A touch target with press feedback; content goes inside
MouseArea {
    id: area
    property color baseColor: "transparent"
    property color pressColor: Theme.pressed
    property alias radius: bg.radius
    implicitWidth: childrenRect.width
    implicitHeight: childrenRect.height

    Rectangle {
        id: bg
        anchors.fill: parent
        radius: height / 2
        color: area.pressed ? area.pressColor : area.baseColor
        Behavior on color { ColorAnimation { duration: 90 } }
    }
}
