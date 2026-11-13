import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import Quickshell.Services.Greetd

// The login screen (greetd, in its own niri as the greeter user): clock,
// the user, a password prompt with the shell's keyboard; starts
// niri-session and quits, niri quits with it, greetd starts the session.
ShellRoot {
    id: root
    property var users: []           // [{ name, gecos }]
    property int selected: 0
    readonly property var user: users[selected] ?? null
    property string message: ""
    property bool messageIsError: false
    property bool busy: false
    property string pending: ""

    // people: uid 1000-59999 with a login shell; the last one to log in
    // first, else the newest account
    FileView {
        id: passwd
        path: "/etc/passwd"
        onLoaded: {
            const list = []
            for (const line of text().split("\n")) {
                const f = line.split(":")
                if (f.length < 7) continue
                const uid = parseInt(f[2])
                if (uid < 1000 || uid >= 60000 || /nologin|false$/.test(f[6])) continue
                list.push({ name: f[0], gecos: f[4].split(",")[0], uid: uid })
            }
            list.sort((a, b) => b.uid - a.uid)
            root.users = list
            root.pickLast()
        }
    }
    FileView {
        id: last
        path: Quickshell.statePath("greeter.json")
        onAdapterUpdated: writeAdapter()
        onLoaded: root.pickLast()
        JsonAdapter {
            id: lastUser
            property string name: ""
        }
    }
    function pickLast() {
        const i = users.findIndex(u => u.name === lastUser.name)
        if (i >= 0) selected = i
    }

    function login(password) {
        if (!user || busy) return
        busy = true
        message = ""
        pending = password
        Greetd.createSession(user.name)
    }

    Connections {
        target: Greetd
        function onAuthMessage(message, error, responseRequired, echoResponse) {
            if (responseRequired) {
                Greetd.respond(root.pending)
                root.pending = ""
            } else if (message !== "") {
                root.message = message
                root.messageIsError = error
            }
        }
        function onAuthFailure(message) {
            root.busy = false
            root.message = "Wrong password"
            root.messageIsError = true
            Greetd.cancelSession()
        }
        function onReadyToLaunch() {
            lastUser.name = root.user.name
            last.writeAdapter()
            // logind takes the session type from the environment
            Greetd.launch(["niri-session"], ["XDG_SESSION_TYPE=wayland", "XDG_SESSION_DESKTOP=niri",
                                             "XDG_CURRENT_DESKTOP=niri"], true)
        }
        function onError(error) {
            root.busy = false
            root.message = error
            root.messageIsError = true
            Greetd.cancelSession()
        }
    }

    Variants {
        model: Quickshell.screens
        PanelWindow {
            required property var modelData
            screen: modelData
            anchors { top: true; bottom: true; left: true; right: true }
            exclusionMode: ExclusionMode.Ignore
            color: "black"
            WlrLayershell.namespace: "cezanne-greeter"
            WlrLayershell.layer: WlrLayer.Top
            WlrLayershell.keyboardFocus: WlrKeyboardFocus.Exclusive

            SystemClock { id: clock; precision: SystemClock.Minutes }

            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#1c2a44" }
                    GradientStop { position: 1.0; color: "#0b0b10" }
                }
            }

            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                y: parent.height * 0.07
                spacing: 4
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: Qt.formatTime(clock.date, "HH:mm")
                    color: Theme.fg
                    font.family: Theme.font
                    font.pixelSize: 56
                    font.weight: Font.Light
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: Qt.formatDate(clock.date, "dddd, d MMMM")
                    color: Theme.fgDim
                    font.family: Theme.font
                    font.pixelSize: 16
                }
            }

            // the user (tap to switch when there are several)
            Tap {
                anchors.horizontalCenter: parent.horizontalCenter
                y: parent.height * 0.25
                width: who.width + 40
                height: who.height + 20
                radius: 20
                onClicked: if (root.users.length > 1) root.selected = (root.selected + 1) % root.users.length
                Column {
                    id: who
                    anchors.centerIn: parent
                    spacing: 8
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 72; height: 72; radius: 36
                        color: Theme.accent
                        Text {
                            anchors.centerIn: parent
                            text: (root.user?.gecos || root.user?.name || "?").charAt(0).toUpperCase()
                            color: Theme.fg
                            font.family: Theme.font
                            font.pixelSize: 32
                        }
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: root.user ? (root.user.gecos || root.user.name) : ""
                        color: Theme.fg
                        font.family: Theme.font
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }
                }
            }

            // restart / power off
            Row {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 12
                spacing: 8
                Repeater {
                    model: [{ icon: "system-reboot-symbolic", cmd: "reboot" },
                            { icon: "system-shutdown-symbolic", cmd: "poweroff" }]
                    Tap {
                        required property var modelData
                        width: 40; height: 40
                        baseColor: Theme.raised
                        onClicked: Quickshell.execDetached(["systemctl", modelData.cmd])
                        SymIcon { anchors.centerIn: parent; size: 16; name: modelData.icon }
                    }
                }
            }

            PasswordPrompt {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: implicitHeight
                busy: root.busy
                message: root.message
                error: root.messageIsError
                onSubmitted: text => { root.login(text); clear() }
            }
        }
    }
}
