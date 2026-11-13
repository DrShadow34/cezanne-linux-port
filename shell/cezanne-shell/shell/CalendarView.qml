import QtQuick
import Quickshell

// The clock's popup, stacked for portrait: date, month calendar, then the
// notifications
Column {
    id: view
    property var notifications: null
    width: parent ? parent.width : 400
    spacing: 12

    SystemClock { id: clock; precision: SystemClock.Minutes }
    property date shown: new Date(clock.date.getFullYear(), clock.date.getMonth(), 1)

    // ---- date
    Column {
        width: parent.width
        leftPadding: 6
        Text {
            text: Qt.formatDate(clock.date, "dddd")
            color: Theme.fgDim
            font.family: Theme.font
            font.pixelSize: Theme.textSize
        }
        Text {
            text: Qt.formatDate(clock.date, "d MMMM yyyy")
            color: Theme.fg
            font.family: Theme.font
            font.pixelSize: 20
            font.weight: Font.DemiBold
        }
    }

    // ---- month
    Rectangle {
        width: parent.width
        height: month.height + 16
        radius: 18
        color: Theme.raised

        Column {
            id: month
            anchors.top: parent.top
            anchors.topMargin: 8
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - 16

            Item {
                width: parent.width
                height: 36
                Tap {
                    anchors.left: parent.left
                    width: 36; height: 36
                    onClicked: view.shown = new Date(view.shown.getFullYear(), view.shown.getMonth() - 1, 1)
                    SymIcon { anchors.centerIn: parent; size: 16; name: "go-previous-symbolic" }
                }
                Tap {
                    anchors.centerIn: parent
                    width: title.width + 24; height: 32
                    onClicked: view.shown = new Date(clock.date.getFullYear(), clock.date.getMonth(), 1)
                    Text {
                        id: title
                        anchors.centerIn: parent
                        text: Qt.formatDate(view.shown, "MMMM yyyy")
                        color: Theme.fg
                        font.family: Theme.font
                        font.pixelSize: Theme.textSize
                        font.weight: Font.DemiBold
                    }
                }
                Tap {
                    anchors.right: parent.right
                    width: 36; height: 36
                    onClicked: view.shown = new Date(view.shown.getFullYear(), view.shown.getMonth() + 1, 1)
                    SymIcon { anchors.centerIn: parent; size: 16; name: "go-next-symbolic" }
                }
            }

            Grid {
                id: days
                columns: 7
                width: parent.width
                readonly property real cell: width / 7
                // Monday first
                readonly property int lead: (view.shown.getDay() + 6) % 7
                readonly property int count: new Date(view.shown.getFullYear(), view.shown.getMonth() + 1, 0).getDate()

                Repeater {
                    model: ["M", "T", "W", "T", "F", "S", "S"]
                    Text {
                        required property string modelData
                        width: days.cell; height: 28
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: modelData
                        color: Theme.fgFaint
                        font.family: Theme.font
                        font.pixelSize: Theme.smallText
                    }
                }
                Repeater {
                    model: 42
                    Item {
                        required property int index
                        readonly property int day: index - days.lead + 1
                        readonly property bool inMonth: day >= 1 && day <= days.count
                        readonly property bool today: inMonth && day === clock.date.getDate()
                            && view.shown.getMonth() === clock.date.getMonth()
                            && view.shown.getFullYear() === clock.date.getFullYear()
                        visible: index < Math.ceil((days.lead + days.count) / 7) * 7
                        width: days.cell; height: 34
                        Rectangle {
                            anchors.centerIn: parent
                            width: 30; height: 30; radius: 15
                            color: parent.today ? Theme.accent : "transparent"
                        }
                        Text {
                            anchors.centerIn: parent
                            text: new Date(view.shown.getFullYear(), view.shown.getMonth(), parent.day).getDate()
                            color: parent.today ? Theme.accentFg : parent.inMonth ? Theme.fg : Theme.fgFaint
                            opacity: parent.inMonth ? 1 : 0.5
                            font.family: Theme.font
                            font.pixelSize: Theme.textSize - 1
                        }
                    }
                }
            }
        }
    }

    // ---- notifications
    Item {
        width: parent.width
        height: 32
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: (view.notifications?.count ?? 0) > 0 ? "Notifications" : "No Notifications"
            color: Theme.fgDim
            font.family: Theme.font
            font.pixelSize: Theme.textSize
        }
        Tap {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            visible: (view.notifications?.count ?? 0) > 0
            width: clearText.width + 28; height: 32
            baseColor: Theme.raised
            onClicked: view.notifications.clearAll()
            Text {
                id: clearText
                anchors.centerIn: parent
                text: "Clear"
                color: Theme.fg
                font.family: Theme.font
                font.pixelSize: Theme.textSize - 1
            }
        }
    }

    Flickable {
        width: parent.width
        height: Math.min(list.height, 520)
        contentHeight: list.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Column {
            id: list
            width: parent.width
            spacing: 8
            Repeater {
                model: view.notifications ? view.notifications.list : []
                NotificationCard {
                    required property var modelData
                    width: list.width
                    notification: modelData
                    onActivated: ShellState.close()
                }
            }
        }
    }
}
