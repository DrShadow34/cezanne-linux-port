import QtQuick

// A password field with the shell's keyboard under it (lock screen, greeter,
// polkit). Hardware keyboards type into the field directly.
Item {
    id: prompt
    property string placeholder: "Password"
    property string message: ""
    property bool error: false
    property bool busy: false
    property bool echo: false
    property alias digits: keys.digits
    property alias text: field.text
    signal submitted(string text)
    // Escape on a hardware keyboard
    signal cancelled()

    function clear() { field.text = "" }
    function submit() { if (!busy) submitted(field.text) }

    implicitHeight: box.height + msg.height + keys.implicitHeight + 32

    Column {
        anchors.bottom: keys.top
        anchors.bottomMargin: 16
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width - 48, 380)
        spacing: 10

        Text {
            id: msg
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: prompt.message
            visible: text !== ""
            wrapMode: Text.Wrap
            color: prompt.error ? "#ff7b63" : Theme.fgDim
            font.family: Theme.font
            font.pixelSize: Theme.textSize
        }
        Rectangle {
            id: box
            width: parent.width
            height: 48
            radius: 24
            color: "#40ffffff"
            border.width: field.activeFocus ? 2 : 0
            border.color: Theme.accent
            TextInput {
                id: field
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                verticalAlignment: TextInput.AlignVCenter
                horizontalAlignment: TextInput.AlignHCenter
                echoMode: prompt.echo ? TextInput.Normal : TextInput.Password
                passwordCharacter: "●"
                color: Theme.fg
                font.family: Theme.font
                font.pixelSize: 18
                // only while shown: a hidden prompt (the lock screen's
                // clock page, the polkit dialog between requests) holding
                // the focus was enabled again as the window's text input
                // at the next lock, and squeekboard came up under the lock
                // screen (it flashed on unlock)
                focus: prompt.visible
                enabled: !prompt.busy
                onAccepted: prompt.submit()
                Keys.onEscapePressed: prompt.cancelled()
                Text {
                    anchors.fill: parent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    visible: field.text === ""
                    text: prompt.busy ? "…" : prompt.placeholder
                    color: Theme.fgFaint
                    font.family: Theme.font
                    font.pixelSize: Theme.textSize + 1
                }
            }
        }
    }

    Keyboard {
        id: keys
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: implicitHeight
        onTyped: t => { if (!prompt.busy) field.insert(field.cursorPosition, t) }
        onBackspace: if (field.cursorPosition > 0) field.remove(field.cursorPosition - 1, field.cursorPosition)
        onEnter: prompt.submit()
    }
}
