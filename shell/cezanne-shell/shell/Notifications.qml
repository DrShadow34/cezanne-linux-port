import QtQuick
import Quickshell
import Quickshell.Wayland
import Quickshell.Services.Notifications

// The notification server (org.freedesktop.Notifications) and its banners.
// All notifications stay in the calendar popup's list until dismissed.
Scope {
    id: root
    readonly property var list: server.trackedNotifications.values.slice().reverse()
    readonly property int count: server.trackedNotifications.values.length
    // a new one, for the notification light (NotificationLight.qml)
    signal added(var notification)
    property var current: null

    function clearAll() {
        for (const n of server.trackedNotifications.values.slice()) n.dismiss()
    }

    NotificationServer {
        id: server
        keepOnReload: true
        bodySupported: true
        actionsSupported: true
        imageSupported: true
        persistenceSupported: true
        onNotification: n => {
            n.tracked = true
            root.added(n)
            const critical = n.urgency === NotificationUrgency.Critical
            // low urgency: in the list only, no banner (as GNOME)
            if (n.urgency === NotificationUrgency.Low) return
            if (n.transient && !critical && ShellState.locked) return
            if ((ShellState.dnd || ShellState.fullscreen) && !critical) return
            root.current = n
            const t = n.expireTimeout > 0 ? n.expireTimeout : 5000
            if (critical) hide.stop(); else { hide.interval = t; hide.restart() }
        }
    }

    Timer {
        id: hide
        onTriggered: root.current = null
    }
    Connections {
        target: root.current
        function onClosed() { root.current = null }
    }

    // the banner below the panel
    PanelWindow {
        screen: Quickshell.screens[0]
        anchors { top: true; left: true; right: true }
        exclusionMode: ExclusionMode.Normal
        implicitHeight: banner.height + 12
        color: "transparent"
        visible: root.current !== null && !ShellState.locked && ShellState.popup === ""
        WlrLayershell.namespace: "cezanne-notification"
        WlrLayershell.layer: WlrLayer.Overlay

        NotificationCard {
            id: banner
            anchors.top: parent.top
            anchors.topMargin: 6
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(parent.width - 16, 440)
            banner: true
            notification: root.current
            onActivated: root.current = null
        }
    }
}
