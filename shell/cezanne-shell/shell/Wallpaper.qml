import QtQuick
import Quickshell
import Quickshell.Wayland

// The wallpaper: GNOME's background setting (light / dark), on the
// background layer (niri keeps it behind the workspaces in the overview)
PanelWindow {
    required property var modelData
    screen: modelData
    anchors { top: true; bottom: true; left: true; right: true }
    exclusionMode: ExclusionMode.Ignore
    color: "#241f31"
    WlrLayershell.namespace: "cezanne-wallpaper"
    WlrLayershell.layer: WlrLayer.Background

    Image {
        anchors.fill: parent
        source: System.wallpaper !== "" ? "file://" + System.wallpaper : ""
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: false
    }
}
