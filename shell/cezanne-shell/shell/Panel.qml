import QtQuick
import Quickshell
import Quickshell.Wayland
import Quickshell.Services.UPower
import Quickshell.Networking
import Quickshell.Bluetooth

// The transparent top bar: workspaces / overview on the left, the clock
// (calendar and notifications) in the middle, status icons (quick menu) on
// the right
PanelWindow {
    id: panel
    required property var modelData
    property var notifications: null

    screen: modelData
    anchors { top: true; left: true; right: true }
    implicitHeight: Theme.panelHeight
    exclusiveZone: Theme.panelHeight
    color: "transparent"
    visible: !ShellState.fullscreen
    WlrLayershell.namespace: "cezanne-panel"
    WlrLayershell.layer: WlrLayer.Top

    // a soft shade so white text stays readable on light wallpapers;
    // solid black while a window is right under the panel (GNOME does the
    // same; the shade's clear bottom row showed as a bright line there)
    readonly property bool solid: Niri.windowsOnActive > 0 && !Niri.overview && !Niri.homeFocused
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#73000000" }
            GradientStop { position: 1.0; color: "#00000000" }
        }
    }
    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: panel.solid ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
    }

    SystemClock {
        id: clock
        precision: SystemClock.Minutes
    }

    // ---- left: workspaces, tap for the overview
    Tap {
        id: overviewButton
        anchors.left: parent.left
        anchors.leftMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        width: dots.width + 24
        height: Theme.panelHeight - 6
        baseColor: Niri.overview ? Theme.raised : "transparent"
        onClicked: { ShellState.close(); Niri.toggleOverview() }

        Row {
            id: dots
            anchors.centerIn: parent
            spacing: 5
            Repeater {
                model: Math.max(1, Niri.workspaceList.length)
                Rectangle {
                    required property int index
                    readonly property var ws: Niri.workspaceList[index] ?? null
                    readonly property bool active: ws ? ws.is_active : true
                    width: active ? 26 : 7
                    height: 7
                    radius: 4
                    color: active ? Theme.fg : Theme.fgDim
                    opacity: active ? 1 : 0.7
                    Behavior on width { NumberAnimation { duration: 150 } }
                }
            }
        }
    }

    // ---- middle: clock, tap for calendar and notifications
    Tap {
        anchors.centerIn: parent
        width: clockRow.width + 24
        height: Theme.panelHeight - 6
        baseColor: ShellState.popup === "calendar" ? Theme.raised : "transparent"
        onClicked: ShellState.toggle("calendar")

        Row {
            id: clockRow
            anchors.centerIn: parent
            spacing: 6
            Text {
                text: Qt.formatTime(clock.date, "HH:mm")
                color: Theme.fg
                font.family: Theme.font
                font.pixelSize: Theme.textSize
                font.weight: Font.DemiBold
            }
            // unread notifications, as GNOME's dot
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 6; height: 6; radius: 3
                color: Theme.fg
                visible: (panel.notifications?.count ?? 0) > 0
            }
        }
    }

    // ---- a screencast running (Screenshot.qml): its time, tap to stop
    Tap {
        id: recPill
        anchors.right: statusTap.left
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        visible: ShellState.recording
        width: recRow.width + 20
        height: Theme.panelHeight - 8
        baseColor: "#e01b24"
        pressColor: "#a51d2d"
        onClicked: ShellState.screenshotControl?.stopRecording()
        property int seconds: 0
        Timer {
            running: ShellState.recording
            repeat: true
            triggeredOnStart: true
            interval: 1000
            onTriggered: recPill.seconds = Math.floor((Date.now() - ShellState.recordingSince) / 1000)
        }
        Row {
            id: recRow
            anchors.centerIn: parent
            spacing: 6
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 8; height: 8; radius: 4
                color: "white"
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: Math.floor(recPill.seconds / 60) + ":" + String(recPill.seconds % 60).padStart(2, "0")
                color: "white"
                font.family: Theme.font
                font.pixelSize: Theme.smallText + 1
                font.weight: Font.DemiBold
                font.features: { "tnum": 1 }
            }
        }
    }

    // ---- right: status, tap for the quick menu
    Tap {
        id: statusTap
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        width: status.width + 22
        height: Theme.panelHeight - 6
        baseColor: ShellState.popup === "quick" ? Theme.raised : "transparent"
        onClicked: ShellState.toggle("quick")

        Row {
            id: status
            anchors.centerIn: parent
            spacing: 8

            readonly property var wifi: Networking.devices.values.find(d => d.type === DeviceType.Wifi) ?? null
            readonly property var wifiNet: wifi ? (wifi.networks.values.find(n => n.connected) ?? null) : null
            readonly property var battery: UPower.displayDevice

            SymIcon {
                anchors.verticalCenter: parent.verticalCenter
                size: 16
                name: {
                    if (!Networking.wifiEnabled) return "network-wireless-disabled-symbolic"
                    const n = status.wifiNet
                    if (!n) return "network-wireless-offline-symbolic"
                    const s = n.signalStrength
                    if (s > 0.8) return "network-wireless-signal-excellent-symbolic"
                    if (s > 0.55) return "network-wireless-signal-good-symbolic"
                    if (s > 0.3) return "network-wireless-signal-ok-symbolic"
                    return "network-wireless-signal-weak-symbolic"
                }
            }
            SymIcon {
                anchors.verticalCenter: parent.verticalCenter
                size: 16
                visible: Bluetooth.defaultAdapter?.enabled ?? false
                name: "bluetooth-active-symbolic"
            }
            SymIcon {
                anchors.verticalCenter: parent.verticalCenter
                size: 16
                visible: ShellState.dnd
                name: "notifications-disabled-symbolic"
            }
            SymIcon {
                anchors.verticalCenter: parent.verticalCenter
                size: 16
                visible: ShellState.caffeine
                name: "icons/caffeine-symbolic.svg"
            }
            SymIcon {
                anchors.verticalCenter: parent.verticalCenter
                size: 16
                visible: status.battery?.isPresent ?? false
                name: status.battery?.iconName || "battery-missing-symbolic"
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: status.battery?.isPresent ?? false
                // the charging type and power: in the quick menu only
                text: Math.round((status.battery?.percentage ?? 0) * 100) + "%"
                color: Theme.fg
                font.family: Theme.font
                font.pixelSize: Theme.smallText + 1
                font.weight: Font.DemiBold
            }
        }
    }

    // shaded with the rest of the screen while a menu is open; still
    // tappable (the same button closes it, another one switches)
    Rectangle {
        anchors.fill: parent
        color: Theme.scrim
        opacity: ["quick", "calendar", "power", "usb"].indexOf(ShellState.popup) >= 0 ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
    }
    // the launcher's own shade (it lies below the panel's exclusive zone)
    Rectangle {
        anchors.fill: parent
        color: Theme.launcherShade
        opacity: ShellState.popup === "launcher" ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 160 } }
    }
}
