import QtQuick
import Quickshell
import Quickshell.Wayland

// The app launcher: a full-screen grid of every app (4 columns in portrait,
// 8 in landscape, like cezanne-app-grid), search on top (the on-screen
// keyboard comes up when it is tapped), long press pins an app to the dock
PanelWindow {
    id: win
    required property var modelData
    readonly property bool wanted: ShellState.popup === "launcher"

    screen: modelData
    anchors { top: true; bottom: true; left: true; right: true }
    exclusionMode: ExclusionMode.Normal
    color: "transparent"
    visible: wanted || fade.running
    WlrLayershell.namespace: "cezanne-launcher"
    WlrLayershell.layer: WlrLayer.Overlay
    // exclusive while open: with on-demand focus niri hands the keyboard back
    // to the window below at the first tap outside this surface, and the
    // on-screen keyboard's keys are such taps (they typed into that window)
    WlrLayershell.keyboardFocus: wanted ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

    onWantedChanged: if (!wanted) { search.text = ""; search.focus = false; grid.contentY = 0 }
    Connections {
        target: ShellState
        function onLauncherQueryChanged() {
            if (ShellState.launcherQuery === "") return
            search.text = ShellState.launcherQuery
            ShellState.launcherQuery = ""
        }
    }

    readonly property var apps: {
        const q = search.text.trim().toLowerCase()
        return DesktopEntries.applications.values
            .filter(e => !e.noDisplay && !e.runInTerminal)
            // keywords is a QML sequence (QStringList), not a JS array:
            // anything that isn't a string becomes one through Array.from
            .filter(e => q === "" || [e.name, e.genericName, e.keywords, e.id, e.comment]
                .some(s => (typeof s === "string" ? s : Array.from(s ?? []).join(" ")).toLowerCase().includes(q)))
            .sort((a, b) => a.name.localeCompare(b.name))
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.launcherShade
        opacity: win.wanted ? 1 : 0
        Behavior on opacity { NumberAnimation { id: fade; duration: 160 } }

        // a tap beside the search field or the icons closes the launcher
        MouseArea {
            anchors.fill: parent
            onClicked: ShellState.close()
        }

        // ---- search
        Rectangle {
            id: field
            anchors.top: parent.top
            anchors.topMargin: 14
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(parent.width - 32, 420)
            height: 44
            radius: 22
            color: Theme.raised
            SymIcon {
                id: lens
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                size: 16
                name: "system-search-symbolic"
                color: Theme.fgDim
            }
            TextInput {
                id: search
                anchors.left: lens.right
                anchors.leftMargin: 10
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.fg
                font.family: Theme.font
                font.pixelSize: Theme.textSize + 1
                onAccepted: if (win.apps.length > 0) { System.launch(win.apps[0]); ShellState.close() }
                Text {
                    anchors.fill: parent
                    visible: search.text === ""
                    text: "Search apps"
                    color: Theme.fgFaint
                    font: search.font
                }
            }
        }

        // ---- grid
        GridView {
            id: grid
            anchors.top: field.bottom
            anchors.topMargin: 16
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 8
            anchors.horizontalCenter: parent.horizontalCenter
            readonly property int columns: win.width > win.height ? 8 : 4
            width: parent.width - 16
            cellWidth: width / columns
            cellHeight: 108
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: ScriptModel { values: win.apps }
            // a tap on the grid's empty space (below the last row)
            TapHandler { onTapped: ShellState.close() }

            delegate: Tap {
                id: cellTap
                required property var modelData
                width: grid.cellWidth
                height: grid.cellHeight
                radius: 18
                onClicked: { System.launch(modelData); ShellState.close() }
                onPressAndHold: ShellState.togglePin(modelData.id)

                Image {
                    id: icon
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top
                    anchors.topMargin: 10
                    width: 56; height: 56
                    sourceSize: Qt.size(56, 56)
                    source: System.iconFor(cellTap.modelData)
                    asynchronous: true
                }
                // pinned to the dock
                Rectangle {
                    visible: ShellState.isPinned(cellTap.modelData.id)
                    anchors.right: icon.right
                    anchors.top: icon.top
                    anchors.margins: -4
                    width: 18; height: 18; radius: 9
                    color: Theme.accent
                    SymIcon { anchors.centerIn: parent; size: 10; name: "view-pin-symbolic" }
                }
                Text {
                    anchors.top: icon.bottom
                    anchors.topMargin: 6
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width - 8
                    horizontalAlignment: Text.AlignHCenter
                    text: cellTap.modelData.name
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    color: Theme.fg
                    font.family: Theme.font
                    font.pixelSize: Theme.smallText
                }
            }
        }
    }
}
