import QtQuick
import Quickshell
import Quickshell.Services.Notifications

// One notification: app, summary, body, actions; tap runs its default
// action, the cross dismisses it
Rectangle {
    id: card
    property var notification: null
    property bool banner: false
    signal activated()

    readonly property var actions: notification ? notification.actions.filter(a => a.identifier !== "default") : []
    readonly property var defaultAction: notification ? (notification.actions.find(a => a.identifier === "default") ?? null) : null
    readonly property var entry: notification && notification.desktopEntry !== "" ? DesktopEntries.byId(notification.desktopEntry) : null

    implicitHeight: col.height + 24
    radius: 18
    color: banner ? Theme.card : Theme.raised

    Tap {
        anchors.fill: parent
        radius: card.radius
        onClicked: {
            if (card.defaultAction) card.defaultAction.invoke()
            else if (card.entry) System.launch(card.entry)
            card.activated()
        }
    }

    Column {
        id: col
        anchors.top: parent.top
        anchors.topMargin: 12
        anchors.left: parent.left
        anchors.leftMargin: 14
        anchors.right: parent.right
        anchors.rightMargin: 44
        spacing: 3

        Row {
            spacing: 6
            Image {
                anchors.verticalCenter: parent.verticalCenter
                width: 16; height: 16
                sourceSize: Qt.size(16, 16)
                source: {
                    const n = card.notification
                    if (!n) return ""
                    if (n.appIcon !== "") return n.appIcon.startsWith("/") ? "file://" + n.appIcon : Quickshell.iconPath(n.appIcon, true)
                    return card.entry ? Quickshell.iconPath(card.entry.icon, true) : ""
                }
                visible: source !== ""
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: card.notification ? (card.notification.appName || card.entry?.name || "") : ""
                color: Theme.fgDim
                font.family: Theme.font
                font.pixelSize: Theme.smallText
            }
        }
        Text {
            width: parent.width
            text: card.notification?.summary ?? ""
            elide: Text.ElideRight
            color: Theme.fg
            font.family: Theme.font
            font.pixelSize: Theme.textSize
            font.weight: Font.DemiBold
        }
        Text {
            width: parent.width
            visible: text !== ""
            text: card.notification?.body ?? ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: card.banner ? 2 : 4
            elide: Text.ElideRight
            color: Theme.fgDim
            font.family: Theme.font
            font.pixelSize: Theme.textSize - 1
        }
        Row {
            visible: card.actions.length > 0
            spacing: 8
            topPadding: 6
            Repeater {
                model: card.actions
                Tap {
                    required property var modelData
                    width: actText.width + 28; height: 34
                    baseColor: Theme.raised
                    onClicked: { modelData.invoke(); card.activated() }
                    Text {
                        id: actText
                        anchors.centerIn: parent
                        text: modelData.text
                        color: Theme.fg
                        font.family: Theme.font
                        font.pixelSize: Theme.smallText + 1
                    }
                }
            }
        }
    }

    Tap {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 6
        width: 34; height: 34
        onClicked: card.notification?.dismiss()
        SymIcon { anchors.centerIn: parent; size: 14; name: "window-close-symbolic"; color: Theme.fgDim }
    }
}
