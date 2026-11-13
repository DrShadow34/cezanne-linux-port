import QtQuick
import Quickshell
import Quickshell.Wayland

// The menus that drop from the panel: quick menu, calendar with
// notifications, power menu. A full-screen layer below the panel; tapping
// outside the card closes it.
PanelWindow {
    id: win
    required property var modelData
    property var notifications: null
    readonly property bool wanted: ["quick", "calendar", "power", "usb"].indexOf(ShellState.popup) >= 0
    // what the card shows: kept while it fades out (else it shrank to an
    // empty dark box first)
    property string shown: ""
    Connections {
        target: ShellState
        function onPopupChanged() { if (win.wanted) win.shown = ShellState.popup }
    }
    Timer { running: !win.wanted && win.shown !== ""; interval: 200; onTriggered: win.shown = "" }

    screen: modelData
    anchors { top: true; bottom: true; left: true; right: true }
    exclusionMode: ExclusionMode.Normal
    color: "transparent"
    visible: wanted || fade.running
    WlrLayershell.namespace: "cezanne-popup"
    // above fullscreen windows too (niri draws those over the top layer)
    WlrLayershell.layer: WlrLayer.Overlay
    // exclusive while open: with on-demand focus niri hands the keyboard back
    // to the window below at the first tap outside this surface, and the
    // on-screen keyboard's keys are such taps (they typed into that window)
    WlrLayershell.keyboardFocus: wanted ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

    Rectangle {
        id: scrim
        anchors.fill: parent
        color: Theme.scrim
        opacity: win.wanted ? 1 : 0
        Behavior on opacity { NumberAnimation { id: fade; duration: 150 } }
        MouseArea {
            anchors.fill: parent
            onClicked: ShellState.close()
        }
    }

    Rectangle {
        id: card
        anchors.top: parent.top
        anchors.topMargin: 6
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width - 16, 440)
        height: Math.min((content.item ? content.item.implicitHeight : 0) + 2 * Theme.pad, parent.height - 12)
        radius: Theme.radius
        color: Theme.card
        opacity: win.wanted ? 1 : 0
        scale: win.wanted ? 1 : 0.96
        transformOrigin: Item.Top
        Behavior on opacity { NumberAnimation { duration: 150 } }
        Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
        clip: true

        // swallow taps on the card
        MouseArea { anchors.fill: parent }

        // scrolls only when the content is taller than the screen allows
        // (landscape)
        Flickable {
            id: scroller
            anchors.fill: parent
            anchors.margins: Theme.pad
            contentHeight: content.item ? content.item.implicitHeight : 0
            flickableDirection: Flickable.VerticalFlick
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height
            Loader {
                id: content
                width: scroller.width
                sourceComponent: win.shown === "quick" ? quick
                               : win.shown === "calendar" ? calendar
                               : win.shown === "power" ? power
                               : win.shown === "usb" ? usbDialog : null
                onSourceComponentChanged: scroller.contentY = 0
            }
        }
    }

    Component { id: quick; QuickMenu {} }
    Component { id: calendar; CalendarView { notifications: win.notifications } }
    Component { id: power; PowerMenu {} }
    Component { id: usbDialog; UsbDialog {} }
}
