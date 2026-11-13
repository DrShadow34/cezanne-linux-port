import QtQuick

// A quick settings pill: icon, title, subtitle; an arrow when it has a list
Rectangle {
    id: root
    property string icon
    property string title
    property string subtitle: ""
    property bool active: false
    property bool expandable: false
    property bool expanded: false
    signal toggled()
    signal expand()

    implicitHeight: 54
    radius: height / 2
    color: active ? Theme.accent : Theme.raised
    Behavior on color { ColorAnimation { duration: 120 } }

    Tap {
        id: main
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: root.expandable ? arrow.left : parent.right
        radius: root.radius
        onClicked: root.toggled()

        SymIcon {
            id: ic
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            size: 18
            name: root.icon
            color: root.active ? Theme.accentFg : Theme.fg
        }
        Column {
            anchors.left: ic.right
            anchors.leftMargin: 10
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            Text {
                width: parent.width
                text: root.title
                elide: Text.ElideRight
                color: root.active ? Theme.accentFg : Theme.fg
                font.family: Theme.font
                font.pixelSize: Theme.textSize
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                visible: root.subtitle !== ""
                text: root.subtitle
                elide: Text.ElideRight
                color: root.active ? Theme.accentFg : Theme.fgDim
                font.family: Theme.font
                font.pixelSize: Theme.smallText
            }
        }
    }
    Tap {
        id: arrow
        visible: root.expandable
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.expandable ? 46 : 0
        radius: root.radius
        onClicked: root.expand()
        Rectangle {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 1; height: parent.height - 24
            color: root.active ? "#55ffffff" : "#33ffffff"
        }
        SymIcon {
            anchors.centerIn: parent
            size: 16
            name: root.expanded ? "go-down-symbolic" : "go-next-symbolic"
            color: root.active ? Theme.accentFg : Theme.fg
        }
    }
}
