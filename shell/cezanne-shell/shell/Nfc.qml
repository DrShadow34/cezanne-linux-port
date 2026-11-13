import QtQuick
import Quickshell
import Quickshell.Io

// NFC (cezanne-nfc's, as Android's switch): while NFC is on and the session
// is unlocked, neard (org.neard on the system bus, D-Bus activated) polls as
// reader on the first adapter (/org/neard/nfcN; nxp-nci on the SN100U);
// off or locked, the adapter is unpowered (VEN low: it draws nothing). A tag
// or card in the field gives a notification (notify-send, to the shell's own
// server): its kind and UID, the NDEF records neard read, "Open" for a URI,
// "Copy" for the text. None while the NFC app (org.cezanne.Nfc) has the
// focus. The on / off choice is the GNOME extension's dconf key.
Scope {
    id: nfc
    // the sandbox leaves the real adapter alone (CEZANNE_NFC=off)
    readonly property bool active: Quickshell.env("CEZANNE_NFC") !== "off"
    readonly property string key: "/org/gnome/shell/extensions/cezanne-nfc/enabled"
    property bool enabled: false
    property bool loaded: false
    property string adapter: ""
    readonly property bool reading: loaded && enabled && adapter !== "" && !ShellState.locked
    readonly property string subtitle: adapter === "" ? "Unavailable"
        : !enabled ? "" : ShellState.locked ? "Off while locked" : "Reading tags"

    function setEnabled(on) {
        enabled = on
        Quickshell.execDetached(["dconf", "write", key, on ? "true" : "false"])
    }
    onReadingChanged: apply()
    onAdapterChanged: apply()
    onLoadedChanged: apply()

    // the field on or off ("already enabled / disabled", "busy": fine)
    function apply() {
        if (!loaded || adapter === "") return
        const a = adapter
        fieldJob.command = ["sh", "-c", reading
            ? 'busctl set-property org.neard "$0" org.neard.Adapter Powered b true 2>/dev/null; busctl call org.neard "$0" org.neard.Adapter StartPollLoop s Initiator 2>/dev/null; true'
            : 'busctl call org.neard "$0" org.neard.Adapter StopPollLoop 2>/dev/null; busctl set-property org.neard "$0" org.neard.Adapter Powered b false 2>/dev/null; true', a]
        if (fieldJob.running) fieldAgain.restart()
        else fieldJob.running = true
    }
    Process { id: fieldJob }
    Timer { id: fieldAgain; interval: 300; onTriggered: nfc.apply() }

    // the setting, then the adapter
    Process {
        running: nfc.active
        command: ["dconf", "read", nfc.key]
        stdout: StdioCollector {
            onStreamFinished: {
                // the extension's default: on
                nfc.enabled = text.trim() !== "false"
                nfc.loaded = true
            }
        }
    }
    Process {
        id: adapters
        running: nfc.active
        command: ["busctl", "--json=short", "call", "org.neard", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects"]
        stdout: StdioCollector {
            onStreamFinished: {
                try {
                    const objects = JSON.parse(text).data[0]
                    const path = Object.keys(objects).sort().find(p => "org.neard.Adapter" in objects[p])
                    if (path) nfc.adapter = path
                } catch (e) {}
            }
        }
    }

    // neard restarts polling after a tag leaves (ConstantPoll); this catches
    // a poll loop that stopped on an error
    Timer {
        running: nfc.reading
        repeat: true
        interval: 10000
        onTriggered: if (Object.keys(nfc.pending).length === 0) nfc.apply()
    }

    // ---- tags: neard's ObjectManager signals as gdbus monitor prints them
    property var pending: ({})    // tag path -> { tag, props, records }
    property var seen: ({})       // uid -> ms last seen
    readonly property var tagTypes: ({
        "Type 1": "NFC Forum Type 1 tag (Topaz)",
        "Type 2": "NFC Forum Type 2 tag (NTAG / MIFARE Ultralight)",
        "Type 3": "NFC Forum Type 3 tag (FeliCa)",
        "Type 4": "NFC Forum Type 4 tag (ISO-DEP)",
        "Type 4A": "NFC Forum Type 4A tag (ISO-DEP)",
        "Type 4B": "NFC Forum Type 4B tag (ISO-DEP)",
        "Type 5": "NFC Forum Type 5 tag (ISO 15693)"
    })
    // a GVariant text value: 'string' or "string" (with a quote inside), bool
    function field(line, name) {
        const m = line.match(new RegExp("'" + name + "': <(?:'((?:[^'\\\\]|\\\\.)*)'|\"((?:[^\"\\\\]|\\\\.)*)\"|(true|false))>"))
        if (!m) return undefined
        return m[3] !== undefined ? m[3] === "true" : (m[1] ?? m[2]).replace(/\\(.)/g, "$1")
    }
    function signal(line) {
        if (line.indexOf("InterfacesAdded") >= 0) {
            const path = (line.match(/objectpath '([^']+)'/) ?? [])[1]
            if (!path) return
            if (line.indexOf("'org.neard.Adapter'") >= 0) {
                if (adapter === "") adapter = path
            } else if (line.indexOf("'org.neard.Tag'") >= 0 || line.indexOf("'org.neard.Device'") >= 0) {
                const uid = (line.match(/'Uid': <\[byte ([^\]]*)\]>/) ?? [])[1]
                const p = Object.assign({}, pending)
                p[path] = {
                    tag: line.indexOf("'org.neard.Tag'") >= 0,
                    props: { type: field(line, "Type"), protocol: field(line, "Protocol"), readOnly: field(line, "ReadOnly"),
                             uid: uid ? uid.split(",").map(b => b.trim().replace(/^0x/, "").padStart(2, "0").toUpperCase()).join(":") : "" },
                    records: []
                }
                pending = p
                reportLater.createObject(nfc, { path: path })
            } else if (line.indexOf("'org.neard.Record'") >= 0) {
                const parent = pending[path.substring(0, path.lastIndexOf("/"))]
                if (parent) parent.records.push({ uri: field(line, "URI"), text: field(line, "Representation"),
                                                  mime: field(line, "MIMEType") ?? field(line, "MIME"), type: field(line, "Type") })
            }
        } else if (line.indexOf("InterfacesRemoved") >= 0 && line.indexOf("'org.neard.Adapter'") >= 0) {
            const path = (line.match(/objectpath '([^']+)'/) ?? [])[1]
            if (path === adapter) adapter = ""
        }
    }
    Process {
        id: monitor
        running: nfc.active
        command: ["gdbus", "monitor", "--system", "--dest", "org.neard"]
        stdout: SplitParser { onRead: line => nfc.signal(line) }
        onExited: monitorAgain.start()
    }
    Timer {
        id: monitorAgain
        interval: 5000
        onTriggered: { monitor.running = true; adapters.running = true }
    }

    // records come in after their tag
    Component {
        id: reportLater
        Timer {
            property string path
            running: true
            interval: 700
            onTriggered: {
                const p = Object.assign({}, nfc.pending)
                const t = p[path]
                delete p[path]
                nfc.pending = p
                if (t) nfc.report(t)
                destroy()
            }
        }
    }

    function report(t) {
        const now = Date.now()
        const key = t.props.uid || (t.tag ? t.props.type + "/" + t.props.protocol : "device")
        const s = {}
        for (const k in seen) if (now - seen[k] <= 4000) s[k] = seen[k]
        const known = key in s
        s[key] = now
        seen = s
        // the same tag again in one sitting; a re-read with records still counts
        if (known && t.records.length === 0) return

        let title
        const body = []
        if (!t.tag) {
            title = "NFC device nearby"
        } else if (t.records.length === 0 && t.props.protocol === "ISO-DEP") {
            title = "Contactless card"
            body.push("ISO-DEP card without NFC data (payment, transit or ID card)")
        } else {
            title = t.records.length ? "NFC tag" : "Empty NFC tag"
            body.push(tagTypes[t.props.type] ?? `${t.props.type ?? "Unknown"} tag (${t.props.protocol})`)
        }
        if (t.props.uid) body.push("UID " + t.props.uid)
        let uri = ""
        for (const r of t.records) {
            if (r.uri) { if (!uri) uri = r.uri; body.push(r.uri) }
            else if (r.text) body.push(r.text)
            else if (r.mime) body.push(r.mime + " data")
            else if (r.type) body.push(r.type + " record")
        }
        if (t.tag && t.props.readOnly && t.records.length) body.push("Read-only")
        console.info("NFC:", title + ":", body.join(", "))
        if (Niri.allWindows.some(w => w.is_focused && w.app_id === "org.cezanne.Nfc")) return
        const text = t.records.map(r => r.uri || r.text).filter(x => x).join("\n")
        notifier.createObject(nfc, { title: title, body: body.join("\n"), uri: uri, text: text })
    }

    // notify-send waits for an action while actions are offered
    Component {
        id: notifier
        Process {
            id: n
            property string title
            property string body
            property string uri
            property string text
            running: true
            command: ["notify-send", "-a", "NFC", "-i", Quickshell.shellPath("icons/nfc-symbolic.svg")]
                .concat(uri !== "" ? ["--action=open=Open"] : [])
                .concat(text !== "" ? ["--action=copy=Copy"] : [])
                .concat([title, body])
            stdout: SplitParser {
                onRead: line => {
                    if (line === "open") Niri.action(["spawn", "--", "xdg-open", n.uri])
                    else if (line === "copy") Quickshell.clipboardText = n.text
                }
            }
            onExited: destroy()
        }
    }
}
