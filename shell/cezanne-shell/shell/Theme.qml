pragma Singleton
import QtQuick
import Quickshell

// Colours and sizes, close to GNOME Shell's dark look
Singleton {
    readonly property string font: "Adwaita Sans"
    readonly property color fg: "#ffffff"
    readonly property color fgDim: "#c0bfbc"
    readonly property color fgFaint: "#9a9996"
    readonly property color card: "#f0222226"
    readonly property color raised: "#33ffffff"
    readonly property color pressed: "#55ffffff"
    readonly property color accent: "#3584e4"
    readonly property color accentFg: "#ffffff"
    readonly property color danger: "#e01b24"
    readonly property color scrim: "#66000000"
    readonly property color turbo: "#f8e45c"     // GNOME palette yellow 2: charge pump
    readonly property color launcherShade: "#d9101014"

    readonly property int panelHeight: 32
    readonly property int radius: 22
    readonly property int pillRadius: 999
    readonly property int pad: 12
    readonly property int dockIcon: 52
    readonly property int textSize: 14
    readonly property int smallText: 12
}
