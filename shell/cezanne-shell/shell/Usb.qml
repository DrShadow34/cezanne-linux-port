import QtQuick
import Quickshell
import Quickshell.Io

// "Use USB for" (cezanne-usb's extension, as on a phone): when a computer
// (not a charger) enumerates the USB-C port, ask whether it is for a video
// stream (the phone as a UVC webcam: the screen or a camera), data storage
// (MTP, the home folder), the developer network (RNDIS, 172.16.42.1) or
// charging only (also when the dialog is dismissed). The dialog only comes
// up unlocked with nothing fullscreen; otherwise the port goes to charge
// only and a notification offers the other choices (an undecided port was
// re-enumerated on every doze). The same when the phone locks with the
// dialog open. While connected, a resident notification (low urgency: no
// banner) shows the mode and opens the dialog again.
// cezanne-usbd (org.cezanne.Usb, system bus) owns the gadget and tells a
// computer from a charger; SetMode is allowed for the active session
// (polkit). Stream mode runs the user unit cezanne-usb-stream.service; for
// the screen the shell keeps the phone from idling (shell.qml's inhibitor).
Scope {
    id: usb
    readonly property var modes: ({
        stream: ["Video stream", "The phone as a webcam: screen or a camera (OBS)", "camera-web-symbolic"],
        storage: ["Data storage", "Files in your home folder (MTP)", "folder-symbolic"],
        network: ["Developer network", "USB network, the phone at 172.16.42.1", "network-wired-symbolic"],
        charge: ["Charge only", "No data connection", "battery-full-charging-symbolic"]
    })
    readonly property string icon: "drive-removable-media-symbolic"
    // the sandbox: the real daemon is read, never set (CEZANNE_USB=off)
    readonly property bool live: Quickshell.env("CEZANNE_USB") !== "off"

    // cezanne-usbd's properties
    property bool host: false
    property string mode: ""
    property string source: ""
    property var sources: []        // [[id, label, camera]]
    readonly property bool screenStream: host && mode === "stream" && source === "screen"

    property string page: "modes"   // the dialog: "modes" / "sources"
    property bool picked: false
    property bool announce: false
    property string notificationId: ""

    // ---- the daemon: all properties as JSON, again on every change
    function refresh() { if (!props.running) props.running = true; else again.restart() }
    Process {
        id: props
        running: true
        command: ["busctl", "--json=short", "call", "org.cezanne.Usb", "/org/cezanne/Usb",
                  "org.freedesktop.DBus.Properties", "GetAll", "s", "org.cezanne.Usb"]
        stdout: StdioCollector {
            onStreamFinished: {
                let p = null
                try { p = JSON.parse(text).data[0] } catch (e) {}
                if (!p) { usb.update(false, "", "", []); return }
                usb.update(p.Plugged.data && p.Host.data, p.Mode.data, p.Source.data, p.Sources.data)
            }
        }
    }
    Timer { id: again; interval: 150; onTriggered: usb.refresh() }
    Process {
        id: watch
        running: true
        command: ["gdbus", "monitor", "--system", "--dest", "org.cezanne.Usb", "--object-path", "/org/cezanne/Usb"]
        stdout: SplitParser { onRead: usb.refresh() }
        onExited: watchAgain.start()
    }
    Timer { id: watchAgain; interval: 5000; onTriggered: { watch.running = true; usb.refresh() } }

    // the extension's _sync
    function update(h, m, s, list) {
        sources = list
        if (h && !host && m === "none") ask()
        if (!h || (m !== mode && m !== "none")) closeDialog()  // unplugged, or decided elsewhere
        if (!h) closeNotification()
        if (m !== mode || s !== source) syncStream(m, s)
        if (h && m !== "none" && (m !== mode || s !== source)) notifyMode(m, s)
        host = h
        mode = h ? m : ""
        source = h ? s : ""
    }

    function ask() {
        if (ShellState.locked || ShellState.fullscreen) chargeOnly()
        else openDialog("modes")
    }
    // not now: charge only, announced with a banner (notifyMode)
    function chargeOnly() {
        announce = true
        setMode("charge", "")
    }
    function openDialog(p) {
        if (ShellState.locked) return
        closeNotification()
        page = p
        picked = false
        ShellState.popup = "usb"
    }
    function closeDialog() { if (ShellState.popup === "usb") { picked = true; ShellState.close() } }
    function pick(m, s) {
        picked = true
        ShellState.close()
        setMode(m, s)
    }
    // dismissed (a tap beside it, back, the lock): charge only when it was
    // undecided, else the mode's notification again (opening took it away)
    Connections {
        target: ShellState
        function onPopupChanged() {
            if (ShellState.popup === "usb" || usb.picked) return
            usb.picked = true
            if (!usb.host) return
            if (usb.mode !== "none") usb.notifyMode(usb.mode, usb.source)
            else if (ShellState.locked) usb.chargeOnly()
            else usb.setMode("charge", "")
        }
    }

    function setMode(m, s) {
        if (!live) { console.info("usb: SetMode", m, s); return }
        setter.createObject(usb, { command: ["busctl", "call", "org.cezanne.Usb", "/org/cezanne/Usb",
                                                               "org.cezanne.Usb", "SetMode", "ss", m, s] })
    }
    Component {
        id: setter
        Process {
            running: true
            stderr: StdioCollector {
                onStreamFinished: if (text.trim() !== "") usb.notify("USB mode failed", text.trim(), { transient: true })
            }
            onExited: destroy()
        }
    }

    function syncStream(m, s) {
        const verb = m === "stream" ? (mode === "stream" ? "restart" : "start")
                   : mode === "stream" ? "stop" : ""
        if (verb !== "" && live) Quickshell.execDetached(["systemctl", "--user", "--no-block", verb, "cezanne-usb-stream.service"])
    }

    // ---- notifications (notify-send to the shell's own server; it waits
    // for an action and prints its name)
    function notifyMode(m, s) {
        let title = modes[m] ? modes[m][0] : m
        if (m === "stream") {
            const src = sources.find(x => x[0] === s)
            title += ": " + (src ? src[1] : s)
        }
        const loud = announce && m === "charge"
        announce = false
        notify(loud ? "USB connected: charging only" : "USB: " + title, "Tap to choose what the USB port is for",
               { resident: true, urgent: loud, actions: m === "charge" })
    }
    function notify(title, body, o) {
        if (!o.transient) closeNotification()
        const args = ["notify-send", "-a", "USB", "-i", icon, "-p",
                      "-u", o.resident && !o.urgent ? "low" : "normal"]
            .concat(o.resident ? ["-h", "boolean:resident:true"] : [])
            .concat(o.transient ? ["-e"] : ["--action=default=Open"])
            .concat(o.actions ? ["--action=stream=" + modes.stream[0], "--action=storage=" + modes.storage[0],
                                 "--action=network=" + modes.network[0]] : [])
            .concat([title, body])
        notifier.createObject(usb, { command: args, own: !o.transient })
    }
    function closeNotification() {
        if (notificationId === "") return
        Quickshell.execDetached(["gdbus", "call", "--session", "--dest", "org.freedesktop.Notifications",
                                 "--object-path", "/org/freedesktop/Notifications",
                                 "--method", "org.freedesktop.Notifications.CloseNotification", notificationId])
        notificationId = ""
    }
    Component {
        id: notifier
        Process {
            id: np
            property bool own: false
            property string nid: ""
            running: true
            stdout: SplitParser {
                onRead: line => {
                    if (np.nid === "") {            // -p: the ID first
                        np.nid = line.trim()
                        if (np.own) usb.notificationId = np.nid
                        return
                    }
                    switch (line.trim()) {
                    case "default": usb.openDialog("modes"); break
                    case "stream": usb.openDialog("sources"); break
                    case "storage": usb.setMode("storage", ""); break
                    case "network": usb.setMode("network", ""); break
                    }
                }
            }
            onExited: destroy()
        }
    }
}
