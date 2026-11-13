import QtQuick

// "Use USB for" (Usb.qml): the modes, then for the video stream its sources
Column {
    id: dialog
    width: parent ? parent.width : 400
    spacing: 8
    readonly property var usb: ShellState.usbControl

    component Choice: Tap {
        id: choice
        property string icon
        property string title
        property string subtitle
        width: dialog.width
        height: 64
        radius: 18
        baseColor: Theme.raised
        SymIcon {
            id: ic
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            size: 22
            name: choice.icon
        }
        Column {
            anchors.left: ic.right
            anchors.leftMargin: 16
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text {
                text: choice.title
                color: Theme.fg
                font.family: Theme.font
                font.pixelSize: Theme.textSize
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                text: choice.subtitle
                color: Theme.fgDim
                elide: Text.ElideRight
                font.family: Theme.font
                font.pixelSize: Theme.smallText
            }
        }
    }

    Text {
        width: parent.width
        topPadding: 4
        horizontalAlignment: Text.AlignHCenter
        text: dialog.usb?.page === "sources" ? "Video stream" : "USB connected"
        color: Theme.fg
        font.family: Theme.font
        font.pixelSize: Theme.textSize + 4
        font.weight: Font.DemiBold
    }
    Text {
        width: parent.width
        bottomPadding: 6
        horizontalAlignment: Text.AlignHCenter
        text: dialog.usb?.page === "sources" ? "The computer sees the phone as a webcam" : "Use USB for"
        color: Theme.fgDim
        font.family: Theme.font
        font.pixelSize: Theme.textSize
    }

    // the modes
    Repeater {
        model: dialog.usb?.page === "modes" ? ["stream", "storage", "network"] : []
        Choice {
            required property string modelData
            icon: dialog.usb.modes[modelData][2]
            title: dialog.usb.modes[modelData][0]
            subtitle: dialog.usb.modes[modelData][1]
            onClicked: {
                if (modelData === "stream") dialog.usb.page = "sources"
                else dialog.usb.pick(modelData, "")
            }
        }
    }
    // the video sources: the screen and the cameras
    Repeater {
        model: dialog.usb?.page === "sources" ? dialog.usb.sources : []
        Choice {
            required property var modelData
            readonly property bool camera: modelData[2] !== "" && modelData[2] !== "false" && modelData[2] !== false
            icon: camera ? "camera-photo-symbolic" : "video-display-symbolic"
            title: modelData[1]
            subtitle: camera ? "Landscape, 1920×1080 or 1280×720" : "Portrait, 1080×2400 or 720×1600"
            onClicked: dialog.usb.pick("stream", modelData[0])
        }
    }

    Row {
        anchors.right: parent.right
        topPadding: 6
        spacing: 8
        Tap {
            visible: dialog.usb?.page === "sources"
            width: backText.implicitWidth + 36
            height: 44
            baseColor: Theme.raised
            onClicked: dialog.usb.page = "modes"
            Text {
                id: backText
                anchors.centerIn: parent
                text: "Back"
                color: Theme.fg
                font.family: Theme.font
                font.pixelSize: Theme.textSize
            }
        }
        Tap {
            width: chargeText.implicitWidth + 36
            height: 44
            baseColor: Theme.accent
            onClicked: dialog.usb.pick("charge", "")
            Text {
                id: chargeText
                anchors.centerIn: parent
                text: "Charge only"
                color: Theme.accentFg
                font.family: Theme.font
                font.pixelSize: Theme.textSize
                font.weight: Font.DemiBold
            }
        }
    }
}
