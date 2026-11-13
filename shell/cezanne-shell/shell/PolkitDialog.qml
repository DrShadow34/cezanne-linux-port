import QtQuick
import Quickshell
import Quickshell.Wayland
import Quickshell.Services.Polkit

// The polkit agent (GNOME Shell was it before): the request's message and a
// password prompt with the shell's keyboard
Scope {
    id: root

    PolkitAgent { id: agent }

    PanelWindow {
        id: dlg
        screen: Quickshell.screens[0]
        anchors { top: true; bottom: true; left: true; right: true }
        exclusionMode: ExclusionMode.Ignore
        color: "#cc000000"
        visible: agent.isActive && agent.flow !== null
        WlrLayershell.namespace: "cezanne-polkit"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: visible ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None

        readonly property var flow: agent.flow

        Column {
            anchors.top: parent.top
            anchors.topMargin: 64
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(parent.width - 48, 400)
            spacing: 14
            SymIcon {
                anchors.horizontalCenter: parent.horizontalCenter
                size: 48
                name: "dialog-password-symbolic"
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "Authentication Required"
                color: Theme.fg
                font.family: Theme.font
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: dlg.flow?.message ?? ""
                wrapMode: Text.Wrap
                color: Theme.fgDim
                font.family: Theme.font
                font.pixelSize: Theme.textSize
            }
            Tap {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 140; height: 42
                baseColor: Theme.raised
                onClicked: dlg.flow?.cancelAuthenticationRequest()
                Text {
                    anchors.centerIn: parent
                    text: "Cancel"
                    color: Theme.fg
                    font.family: Theme.font
                    font.pixelSize: Theme.textSize
                }
            }
        }

        PasswordPrompt {
            id: prompt
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: implicitHeight
            // hidden with the window between requests (its field keeps no
            // focus then, see PasswordPrompt)
            visible: agent.isActive && agent.flow !== null
            readonly property var flow: dlg.flow
            placeholder: flow?.inputPrompt || "Password"
            echo: flow?.responseVisible ?? false
            busy: !(flow?.isResponseRequired ?? false)
            error: flow?.failed || (flow?.supplementaryIsError ?? false)
            message: flow?.failed ? "Sorry, that didn't work" : (flow?.supplementaryMessage ?? "")
            onSubmitted: text => {
                if (flow && flow.isResponseRequired) flow.submit(text)
                clear()
            }
        }
    }
}
