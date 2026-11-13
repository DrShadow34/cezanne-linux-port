import QtQuick
import Quickshell
import Quickshell.Wayland

// The dock: pinned apps, then running ones, then the app grid button. It
// shows on an empty workspace, in the overview and in the launcher; while a
// window covers the screen it hides, and a swipe up from the bottom edge
// brings it back for a few seconds (dash-to-dock's intellihide, by touch).
Scope {
    id: root
    required property var modelData

    readonly property bool shown: !ShellState.fullscreen && !ShellState.locked
        && (Niri.overview || Niri.windowsOnActive === 0 || Niri.homeFocused
            || ShellState.popup === "launcher" || ShellState.dockRevealed)

    // running apps not pinned, in the order they were opened
    readonly property var running: {
        System.apps
        const ids = []
        for (const w of Niri.windowList) {
            const e = System.entryFor(w.app_id)
            const id = e ? e.id : w.app_id
            if (id && ids.indexOf(id) < 0 && !ShellState.isPinned(id)) ids.push(id)
        }
        return ids
    }
    readonly property var items: ShellState.pinned.concat(running)

    function windowsFor(id) {
        return Niri.windowList.filter(w => {
            const e = System.entryFor(w.app_id)
            return (e ? e.id : w.app_id) === id
        })
    }
    function activate(id) {
        ShellState.dockRevealed = false
        const ws = windowsFor(id)
        if (ws.length === 0) {
            System.launchId(id)
            ShellState.close()
            return
        }
        // focused already: the next window of the app
        const i = ws.findIndex(w => w.id === Niri.focusedWindowId)
        Niri.focusWindow(ws[(i + 1) % ws.length].id)
        Niri.closeOverview()
        ShellState.close()
    }

    Timer {
        id: autoHide
        interval: 4000
        onTriggered: ShellState.dockRevealed = false
    }
    Connections {
        target: ShellState
        function onDockRevealedChanged() { if (ShellState.dockRevealed) autoHide.restart() }
    }

    PanelWindow {
        id: dock
        screen: root.modelData
        anchors { bottom: true; left: true; right: true }
        implicitHeight: Theme.dockIcon + 28
        exclusiveZone: 0
        color: "transparent"
        visible: root.shown || slide.running
        WlrLayershell.namespace: "cezanne-dock"
        WlrLayershell.layer: WlrLayer.Top

        Item {
            id: body
            anchors.fill: parent
            anchors.topMargin: root.shown ? 0 : dock.height
            anchors.bottomMargin: root.shown ? 0 : -dock.height
            Behavior on anchors.topMargin { NumberAnimation { id: slide; duration: 180; easing.type: Easing.OutCubic } }
            Behavior on anchors.bottomMargin { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

            Row {
                id: row
                anchors.centerIn: parent
                spacing: 6

                Repeater {
                    model: ScriptModel { values: root.items }
                    Item {
                        id: slot
                        required property string modelData
                        readonly property var entry: { System.apps; return System.entryFor(slot.modelData) }
                        readonly property int count: root.windowsFor(modelData).length
                        width: Theme.dockIcon + 8
                        height: Theme.dockIcon + 16

                        Tap {
                            anchors.fill: parent
                            radius: 16
                            onClicked: root.activate(slot.modelData)
                            onPressAndHold: menu.open(slot)
                            onPressed: autoHide.restart()
                            Image {
                                anchors.centerIn: parent
                                anchors.verticalCenterOffset: -3
                                width: Theme.dockIcon; height: Theme.dockIcon
                                sourceSize: Qt.size(Theme.dockIcon, Theme.dockIcon)
                                source: System.iconFor(slot.entry)
                                asynchronous: true
                            }
                        }
                        // running
                        Row {
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 2
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: 3
                            Repeater {
                                model: Math.min(slot.count, 3)
                                Rectangle { width: 5; height: 5; radius: 2.5; color: Theme.fg }
                            }
                        }
                    }
                }

                // separator and the app grid
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 1; height: Theme.dockIcon - 12
                    color: "#66ffffff"
                }
                Tap {
                    width: Theme.dockIcon + 8
                    height: Theme.dockIcon + 16
                    radius: 16
                    baseColor: ShellState.popup === "launcher" ? Theme.raised : "transparent"
                    onClicked: { ShellState.dockRevealed = false; Niri.closeOverview(); ShellState.toggle("launcher") }
                    Rectangle {
                        anchors.centerIn: parent
                        anchors.verticalCenterOffset: -3
                        width: Theme.dockIcon - 6; height: Theme.dockIcon - 6
                        radius: width / 2
                        color: "#40ffffff"
                        SymIcon { anchors.centerIn: parent; size: 22; name: "view-app-grid-symbolic" }
                    }
                }
            }
        }
    }

    // long press on an app: close its windows, pin / unpin
    PanelWindow {
        id: menu
        property string app: ""
        property real at: 0
        function open(slot) {
            app = slot.modelData
            at = slot.mapToItem(null, slot.width / 2, 0).x
            autoHide.stop()
        }
        screen: root.modelData
        anchors { top: true; bottom: true; left: true; right: true }
        exclusionMode: ExclusionMode.Ignore
        color: "transparent"
        visible: app !== ""
        WlrLayershell.namespace: "cezanne-dock-menu"
        WlrLayershell.layer: WlrLayer.Overlay

        MouseArea {
            anchors.fill: parent
            onClicked: { menu.app = ""; autoHide.restart() }
        }
        Rectangle {
            x: Math.max(8, Math.min(parent.width - width - 8, menu.at - width / 2))
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Theme.dockIcon + 34
            width: 200
            height: menuCol.height + 12
            radius: 18
            color: Theme.card
            Column {
                id: menuCol
                anchors.centerIn: parent
                width: parent.width - 12
                Text {
                    width: parent.width
                    leftPadding: 12; topPadding: 6; bottomPadding: 6
                    text: (DesktopEntries.byId(menu.app) ?? System.entryFor(menu.app))?.name ?? menu.app
                    elide: Text.ElideRight
                    color: Theme.fgDim
                    font.family: Theme.font
                    font.pixelSize: Theme.smallText
                }
                Repeater {
                    model: [
                        { text: "Close", act: "close", show: root.windowsFor(menu.app).length > 0 },
                        { text: root.windowsFor(menu.app).some(w => w.is_floating) ? "Tile Window" : "Float Window",
                          act: "float", show: root.windowsFor(menu.app).length > 0 },
                        { text: ShellState.isPinned(menu.app) ? "Unpin from Dock" : "Pin to Dock", act: "pin", show: true }
                    ]
                    Tap {
                        required property var modelData
                        visible: modelData.show
                        width: menuCol.width
                        height: visible ? 44 : 0
                        radius: 12
                        onClicked: {
                            const ws = root.windowsFor(menu.app)
                            if (modelData.act === "close") for (const w of ws) Niri.closeWindow(w.id)
                            else if (modelData.act === "float") {
                                Niri.action(["toggle-window-floating", "--id", String(ws[0].id)])
                                Niri.focusWindow(ws[0].id)
                            } else ShellState.togglePin(menu.app)
                            menu.app = ""
                        }
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.text
                            color: Theme.fg
                            font.family: Theme.font
                            font.pixelSize: Theme.textSize
                        }
                    }
                }
            }
        }
    }

    // the bottom edge while the dock is hidden: swipe up to show it
    PanelWindow {
        screen: root.modelData
        anchors { bottom: true; left: true; right: true }
        implicitHeight: 10
        exclusiveZone: 0
        color: "transparent"
        visible: !root.shown && !ShellState.fullscreen && !ShellState.locked && ShellState.popup === ""
        WlrLayershell.namespace: "cezanne-dock-edge"
        WlrLayershell.layer: WlrLayer.Top

        MouseArea {
            anchors.fill: parent
            property real startY: 0
            onPressed: mouse => startY = mouse.y
            onPositionChanged: mouse => { if (startY - mouse.y > 12) ShellState.dockRevealed = true }
            onReleased: mouse => { if (startY - mouse.y > 4) ShellState.dockRevealed = true }
        }
    }
}
