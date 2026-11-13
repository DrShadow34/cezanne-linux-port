import QtQuick

// Lock, suspend, restart, power off, log out
Column {
    width: parent ? parent.width : 400
    spacing: 6

    Repeater {
        model: [
            { icon: "system-lock-screen-symbolic", text: "Lock", act: "lock" },
            { icon: "weather-clear-night-symbolic", text: "Suspend", act: "suspend" },
            { icon: "system-reboot-symbolic", text: "Restart", act: "reboot" },
            { icon: "system-shutdown-symbolic", text: "Power Off", act: "poweroff" },
            { icon: "system-log-out-symbolic", text: "Log Out", act: "logout" }
        ]
        Tap {
            required property var modelData
            width: parent.width
            height: 52
            radius: 16
            baseColor: Theme.raised
            onClicked: {
                ShellState.close()
                switch (modelData.act) {
                case "lock": ShellState.lock(); break
                case "suspend": ShellState.sleepRequested(); break
                case "reboot": System.reboot(); break
                case "poweroff": System.poweroff(); break
                case "logout": System.logout(); break
                }
            }
            SymIcon {
                id: pic
                anchors.left: parent.left
                anchors.leftMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                size: 18
                name: modelData.icon
            }
            Text {
                anchors.left: pic.right
                anchors.leftMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                text: modelData.text
                color: Theme.fg
                font.family: Theme.font
                font.pixelSize: Theme.textSize + 1
            }
        }
    }
}
