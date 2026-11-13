import QtQuick
import Quickshell

// A window with a focused text field that logs what reaches it ("TARGET"),
// for keyboard focus tests in the sandbox (run qs -p test/typetarget.qml,
// then type with wtype): a stand-in for an app, so no test types into a
// real app or document.
ShellRoot {
    FloatingWindow {
        implicitWidth: 400
        implicitHeight: 200
        color: "#303030"
        TextInput {
            anchors.fill: parent
            anchors.margins: 20
            focus: true
            color: "white"
            font.pixelSize: 24
            onTextChanged: console.info("TARGET", text)
        }
    }
}
