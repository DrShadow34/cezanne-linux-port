import QtQuick

// The shell's own keyboard for the lock screen, the greeter and polkit
// prompts (niri shows only the lock surface while locked, so squeekboard
// can't come up there). Letters, digits and symbols; one-shot shift.
Rectangle {
    id: kb
    property bool digits: false
    property bool symbols: false
    property bool shift: false
    signal typed(string text)
    signal backspace()
    signal enter()

    implicitHeight: 4 * 52 + 5 * 6 + 8
    color: "#e6141418"

    readonly property var letters: [
        ["q", "w", "e", "r", "t", "y", "u", "i", "o", "p"],
        ["a", "s", "d", "f", "g", "h", "j", "k", "l"],
        ["⇧", "z", "x", "c", "v", "b", "n", "m", "⌫"],
        ["123", ",", " ", ".", "⏎"]
    ]
    readonly property var digitRows: [
        ["1", "2", "3", "4", "5", "6", "7", "8", "9", "0"],
        ["-", "/", ":", ";", "(", ")", "$", "&", "@", "\""],
        ["#+=", ".", ",", "?", "!", "'", "*", "⌫"],
        ["ABC", ",", " ", ".", "⏎"]
    ]
    readonly property var symbolRows: [
        ["[", "]", "{", "}", "#", "%", "^", "*", "+", "="],
        ["_", "\\", "|", "~", "<", ">", "€", "£", "¥", "•"],
        ["123", ".", ",", "?", "!", "'", "`", "⌫"],
        ["ABC", ",", " ", ".", "⏎"]
    ]
    readonly property var rows: symbols ? symbolRows : digits ? digitRows : letters

    function press(k) {
        switch (k) {
        case "⇧": shift = !shift; return
        case "⌫": backspace(); return
        case "⏎": enter(); return
        case "123": digits = true; symbols = false; return
        case "#+=": symbols = true; return
        case "ABC": digits = false; symbols = false; return
        }
        typed(shift ? k.toUpperCase() : k)
        shift = false
    }

    Column {
        anchors.fill: parent
        anchors.margins: 4
        anchors.topMargin: 8
        spacing: 6

        Repeater {
            model: kb.rows
            Row {
                id: keyRow
                required property var modelData
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 5
                // a full row has 10 keys; wide keys take what is left
                readonly property real unit: (kb.width - 8 - 9 * 5) / 10

                Repeater {
                    model: keyRow.modelData
                    Tap {
                        required property string modelData
                        readonly property bool wide: ["⇧", "⌫", "123", "#+=", "ABC", "⏎"].indexOf(modelData) >= 0
                        readonly property bool space: modelData === " "
                        width: space ? keyRow.unit * 4 + 15
                             : wide ? (keyRow.modelData.length === 5 ? keyRow.unit * 2 + 5 : keyRow.unit * 1.5 + 2.5)
                             : keyRow.unit
                        height: 52
                        radius: 10
                        baseColor: wide ? "#3d3d42" : "#58585e"
                        pressColor: "#8a8a92"
                        onClicked: kb.press(modelData)

                        Text {
                            anchors.centerIn: parent
                            visible: ["⌫", "⏎", "⇧"].indexOf(parent.modelData) < 0
                            text: parent.space ? "" : (kb.shift && parent.modelData.length === 1 ? parent.modelData.toUpperCase() : parent.modelData)
                            color: Theme.fg
                            font.family: Theme.font
                            font.pixelSize: parent.wide ? Theme.textSize : 20
                        }
                        SymIcon {
                            anchors.centerIn: parent
                            visible: ["⌫", "⏎", "⇧"].indexOf(parent.modelData) >= 0
                            size: 18
                            name: parent.modelData === "⌫" ? "edit-clear-symbolic"
                                : parent.modelData === "⏎" ? "go-next-symbolic" : "go-up-symbolic"
                            color: parent.modelData === "⇧" && kb.shift ? Theme.accent : Theme.fg
                        }
                    }
                }
            }
        }
    }
}
