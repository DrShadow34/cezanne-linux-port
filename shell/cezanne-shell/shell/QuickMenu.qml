import QtQuick
import Quickshell
import Quickshell.Networking
import Quickshell.Bluetooth
import Quickshell.Io
import Quickshell.Services.UPower
import Quickshell.Services.Pipewire

// Quick settings: battery and actions, brightness and volume, toggles; the
// Wi-Fi and Bluetooth pills open their lists
Column {
    id: menu
    width: parent ? parent.width : 400
    spacing: 10

    property string expanded: ShellState.quickSection    // "", "wifi", "bt"
    readonly property var wifi: Networking.devices.values.find(d => d.type === DeviceType.Wifi) ?? null
    readonly property var wifiNet: wifi ? (wifi.networks.values.find(n => n.connected) ?? null) : null
    readonly property var adapter: Bluetooth.defaultAdapter
    readonly property var sink: Pipewire.defaultAudioSink
    readonly property var battery: UPower.displayDevice
    // airplane mode: every radio soft-blocked (rfkill: Wi-Fi, Bluetooth,
    // NFC) and the modem's radio off (NetworkManager's WWAN switch)
    property bool airplane: false
    function setAirplane(on) {
        airplane = on
        airplaneJob.command = ["sh", "-c", on ? "rfkill block all; nmcli radio wwan off"
                                              : "rfkill unblock all; nmcli radio wwan on"]
        airplaneJob.running = true
    }
    Process {
        id: airplaneJob
        onExited: rfkillRead.running = true
    }
    Process {
        id: rfkillRead
        running: true
        command: ["rfkill", "-n", "-o", "TYPE,SOFT"]
        stdout: StdioCollector {
            onStreamFinished: {
                const blocked = t => text.split("\n").some(l => l.trim().startsWith(t) && l.trim().endsWith(" blocked"))
                menu.airplane = blocked("wlan") && blocked("bluetooth")
            }
        }
    }
    property string chargeKind: "none"   // none / charging / fast / direct (cezanne-aod-ctl)
    property int chargeMw: 0

    Component.onCompleted: System.readBrightness()

    // ---- header: battery, screenshot, settings, lock, power
    Item {
        width: parent.width
        height: 44
        Rectangle {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            height: 40
            width: batRow.width + 28
            radius: 20
            color: Theme.raised
            Row {
                id: batRow
                anchors.centerIn: parent
                spacing: 6
                SymIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    size: 16
                    name: menu.battery?.iconName || "battery-missing-symbolic"
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: Math.round((menu.battery?.percentage ?? 0) * 100) + "%"
                    color: Theme.fg
                    font.family: Theme.font
                    font.pixelSize: Theme.textSize
                }
                // charging: the type as bolts (one 5 V, two fast 9 V, three
                // yellow the charge pump) and the power into the battery
                SymIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: menu.chargeKind !== "none"
                    size: 16
                    name: "icons/charge-" + ({ fast: "fast", direct: "turbo" }[menu.chargeKind] ?? "standard") + "-symbolic.svg"
                    color: menu.chargeKind === "direct" ? Theme.turbo : Theme.fg
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: menu.chargeKind !== "none" && menu.chargeMw >= 100
                    text: (menu.chargeMw / 1000).toFixed(1) + " W"
                    color: Theme.fg
                    font.family: Theme.font
                    font.pixelSize: Theme.textSize
                    font.features: { "tnum": 1 }
                }
            }
        }
        // the charging state, every 3 s while the menu is open
        Process {
            id: chargeRead
            command: [ShellState.aodControl?.ctl ?? "/usr/lib/cezanne-shell/cezanne-aod-ctl", "charge"]
            stdout: StdioCollector {
                onStreamFinished: {
                    const [kind, mw] = text.trim().split(" ")
                    menu.chargeKind = kind || "none"
                    menu.chargeMw = parseInt(mw) || 0
                }
            }
        }
        Timer {
            running: true
            repeat: true
            triggeredOnStart: true
            interval: 3000
            onTriggered: chargeRead.running = true
        }
        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Repeater {
                model: [
                    { icon: "applets-screenshooter-symbolic", act: "shot" },
                    { icon: "emblem-system-symbolic", act: "settings" },
                    { icon: "system-lock-screen-symbolic", act: "lock" },
                    { icon: "system-shutdown-symbolic", act: "power" }
                ]
                Tap {
                    required property var modelData
                    width: 40; height: 40
                    baseColor: Theme.raised
                    onClicked: {
                        switch (modelData.act) {
                        case "shot": ShellState.screenshotControl?.open(); break
                        case "settings": ShellState.close(); System.openSettings(""); break
                        case "lock": ShellState.lock(); break
                        case "power": ShellState.popup = "power"; break
                        }
                    }
                    SymIcon { anchors.centerIn: parent; size: 16; name: modelData.icon }
                }
            }
        }
    }

    // ---- sliders
    // brightness, and "A" for automatic brightness (then the slider is a
    // gain on the light curve: the middle follows it)
    Item {
        width: parent.width
        height: 44
        Slider {
            anchors.left: parent.left
            anchors.right: autoButton.left
            anchors.verticalCenter: parent.verticalCenter
            icon: "display-brightness-symbolic"
            value: System.autoBrightness ? ShellState.autoBias : System.brightnessLevel
            onMoved: v => System.autoBrightness ? System.setAutoBias(v) : System.setBrightnessLevel(v)
        }
        Tap {
            id: autoButton
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 40
            height: 40
            baseColor: System.autoBrightness ? Theme.accent : Theme.raised
            onClicked: System.setAutoBrightness(!System.autoBrightness)
            Text {
                anchors.centerIn: parent
                text: "A"
                color: System.autoBrightness ? Theme.accentFg : Theme.fg
                font.family: Theme.font
                font.pixelSize: Theme.textSize + 2
                font.weight: Font.DemiBold
            }
        }
    }
    Slider {
        width: parent.width
        icon: !menu.sink || menu.sink.audio.muted ? "audio-volume-muted-symbolic"
              : menu.sink.audio.volume > 0.66 ? "audio-volume-high-symbolic"
              : menu.sink.audio.volume > 0.33 ? "audio-volume-medium-symbolic" : "audio-volume-low-symbolic"
        iconActive: !(menu.sink?.audio.muted ?? true)
        value: menu.sink ? Math.min(1, menu.sink.audio.volume) : 0
        onMoved: v => { if (menu.sink) { menu.sink.audio.muted = false; menu.sink.audio.volume = v } }
        onIconClicked: if (menu.sink) menu.sink.audio.muted = !menu.sink.audio.muted
    }

    // ---- Wi-Fi and Bluetooth, their lists right under them
    Grid {
        width: parent.width
        columns: 2
        columnSpacing: 10
        rowSpacing: 10
        readonly property real cell: (width - columnSpacing) / 2

        QuickToggle {
            width: parent.cell
            icon: Networking.wifiEnabled ? "network-wireless-symbolic" : "network-wireless-disabled-symbolic"
            title: "Wi-Fi"
            subtitle: !Networking.wifiEnabled ? "Off" : (menu.wifiNet ? menu.wifiNet.name : "Not connected")
            active: Networking.wifiEnabled
            expandable: true
            expanded: menu.expanded === "wifi"
            onToggled: Networking.wifiEnabled = !Networking.wifiEnabled
            onExpand: menu.expanded = menu.expanded === "wifi" ? "" : "wifi"
        }
        QuickToggle {
            width: parent.cell
            icon: "bluetooth-active-symbolic"
            title: "Bluetooth"
            readonly property var connected: Bluetooth.devices.values.filter(d => d.connected)
            subtitle: !(menu.adapter?.enabled ?? false) ? "Off"
                      : connected.length === 1 ? connected[0].name
                      : connected.length > 1 ? connected.length + " devices" : "On"
            active: menu.adapter?.enabled ?? false
            expandable: menu.adapter !== null
            expanded: menu.expanded === "bt"
            onToggled: if (menu.adapter) menu.adapter.enabled = !menu.adapter.enabled
            onExpand: menu.expanded = menu.expanded === "bt" ? "" : "bt"
        }
    }

    // ---- Wi-Fi networks
    Column {
        width: parent.width
        spacing: 4
        visible: menu.expanded === "wifi" && Networking.wifiEnabled && menu.wifi !== null
        property string asking: ""      // network that needs a password

        onVisibleChanged: if (menu.wifi) menu.wifi.scannerEnabled = visible
        Component.onDestruction: if (menu.wifi) menu.wifi.scannerEnabled = false

        Repeater {
            model: ScriptModel {
                values: menu.wifi ? menu.wifi.networks.values
                    .filter(n => n.name !== "")
                    .sort((a, b) => (b.connected - a.connected) || (b.known - a.known) || (b.signalStrength - a.signalStrength))
                    .slice(0, 12) : []
            }
            delegate: Column {
                id: netRow
                required property var modelData
                width: parent.width
                Tap {
                    width: parent.width
                    height: 44
                    radius: 14
                    onClicked: {
                        if (modelData.connected) return
                        if (modelData.known || modelData.security === WifiSecurityType.Open) modelData.connect()
                        else parent.parent.asking = parent.parent.asking === modelData.name ? "" : modelData.name
                    }
                    SymIcon {
                        id: sig
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        size: 16
                        name: modelData.signalStrength > 0.75 ? "network-wireless-signal-excellent-symbolic"
                            : modelData.signalStrength > 0.5 ? "network-wireless-signal-good-symbolic"
                            : modelData.signalStrength > 0.25 ? "network-wireless-signal-ok-symbolic"
                            : "network-wireless-signal-weak-symbolic"
                    }
                    Text {
                        anchors.left: sig.right
                        anchors.leftMargin: 10
                        anchors.right: state.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.name
                        elide: Text.ElideRight
                        color: Theme.fg
                        font.family: Theme.font
                        font.pixelSize: Theme.textSize
                        font.weight: modelData.connected ? Font.DemiBold : Font.Normal
                    }
                    SymIcon {
                        id: state
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        size: 16
                        name: modelData.connected ? "object-select-symbolic"
                            : modelData.stateChanging ? "content-loading-symbolic"
                            : modelData.security !== WifiSecurityType.Open ? "network-wireless-encrypted-symbolic" : ""
                    }
                }
                // password for a new network (the on-screen keyboard comes up)
                Row {
                    visible: netRow.parent.asking === modelData.name
                    width: parent.width
                    height: visible ? 44 : 0
                    spacing: 8
                    Rectangle {
                        width: parent.width - 96
                        height: 40
                        radius: 12
                        color: Theme.raised
                        TextInput {
                            id: psk
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            verticalAlignment: TextInput.AlignVCenter
                            echoMode: TextInput.Password
                            color: Theme.fg
                            font.family: Theme.font
                            font.pixelSize: Theme.textSize
                            onAccepted: connectBtn.clicked(null)
                        }
                    }
                    Tap {
                        id: connectBtn
                        width: 88; height: 40
                        baseColor: Theme.accent
                        onClicked: {
                            modelData.connectWithPsk(psk.text)
                            psk.text = ""
                            netRow.parent.asking = ""
                        }
                        Text {
                            anchors.centerIn: parent
                            text: "Connect"
                            color: Theme.accentFg
                            font.family: Theme.font
                            font.pixelSize: Theme.textSize
                        }
                    }
                }
            }
        }
    }

    // ---- Bluetooth devices (paired ones; pairing new ones: Settings)
    Column {
        width: parent.width
        spacing: 4
        visible: menu.expanded === "bt" && (menu.adapter?.enabled ?? false)
        Repeater {
            model: ScriptModel { values: Bluetooth.devices.values.filter(d => d.paired) }
            Tap {
                required property var modelData
                width: parent.width
                height: 44
                radius: 14
                onClicked: modelData.connected ? modelData.disconnect() : modelData.connect()
                SymIcon {
                    id: bic
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    size: 16
                    name: (modelData.icon || "bluetooth") + "-symbolic"
                }
                Text {
                    anchors.left: bic.right
                    anchors.leftMargin: 10
                    anchors.right: parent.right
                    anchors.rightMargin: 40
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.name + (modelData.batteryAvailable ? "  " + Math.round(modelData.battery * 100) + "%" : "")
                    elide: Text.ElideRight
                    color: Theme.fg
                    font.family: Theme.font
                    font.pixelSize: Theme.textSize
                    font.weight: modelData.connected ? Font.DemiBold : Font.Normal
                }
                SymIcon {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    size: 16
                    visible: modelData.connected
                    name: "object-select-symbolic"
                }
            }
        }
        Tap {
            width: parent.width
            height: 40
            radius: 14
            onClicked: { ShellState.close(); System.openSettings("bluetooth") }
            Text {
                anchors.centerIn: parent
                text: "Bluetooth Settings"
                color: Theme.fgDim
                font.family: Theme.font
                font.pixelSize: Theme.textSize
            }
        }
    }

    // ---- the other toggles
    Grid {
        width: parent.width
        columns: 2
        columnSpacing: 10
        rowSpacing: 10
        readonly property real cell: (width - columnSpacing) / 2

        QuickToggle {
            width: parent.cell
            icon: ShellState.rotationLock ? "rotation-locked-symbolic" : "rotation-allowed-symbolic"
            title: "Auto Rotate"
            active: !ShellState.rotationLock
            onToggled: ShellState.rotationLock = !ShellState.rotationLock
        }
        QuickToggle {
            width: parent.cell
            icon: "night-light-symbolic"
            title: "Dark Style"
            active: System.dark
            onToggled: System.setDark(!System.dark)
        }
        QuickToggle {
            width: parent.cell
            icon: "notifications-disabled-symbolic"
            title: "Do Not Disturb"
            active: ShellState.dnd
            onToggled: ShellState.dnd = !ShellState.dnd
        }
        QuickToggle {
            width: parent.cell
            icon: "preferences-system-time-symbolic"
            title: "Always-on Display"
            active: ShellState.aodControl?.enabled ?? false
            onToggled: ShellState.aodControl?.setEnabled(!active)
        }
        QuickToggle {
            width: parent.cell
            icon: "icons/caffeine-symbolic.svg"
            title: "Caffeine"
            subtitle: ShellState.caffeine ? "No auto sleep" : ""
            active: ShellState.caffeine
            onToggled: ShellState.caffeine = !ShellState.caffeine
        }
        QuickToggle {
            width: parent.cell
            icon: "input-touchpad-symbolic"
            title: "Show Taps"
            active: ShellState.showTaps
            onToggled: ShellState.showTaps = !ShellState.showTaps
        }
        QuickToggle {
            width: parent.cell
            visible: System.flashLeds.length > 0
            icon: "media-flash-symbolic"
            title: "Flashlight"
            subtitle: ["Low", "Medium", "High"][System.flashLevels.indexOf(ShellState.flashLevel)] ?? ""
            active: System.flashlight
            expandable: true
            expanded: menu.expanded === "flash"
            onToggled: System.setFlashlight(!System.flashlight)
            onExpand: menu.expanded = menu.expanded === "flash" ? "" : "flash"
        }
        QuickToggle {
            width: parent.cell
            icon: "airplane-mode-symbolic"
            title: "Airplane Mode"
            active: menu.airplane
            onToggled: menu.setAirplane(!menu.airplane)
        }
        QuickToggle {
            width: parent.cell
            icon: "icons/nfc-symbolic.svg"
            title: "NFC"
            subtitle: ShellState.nfcControl?.subtitle ?? ""
            active: ShellState.nfcControl?.enabled ?? false
            onToggled: ShellState.nfcControl?.setEnabled(!active)
        }
        QuickToggle {
            width: parent.cell
            icon: "display-brightness-symbolic"
            title: "DC Dimming"
            active: System.dcDimming
            onToggled: System.setDcDimming(!System.dcDimming)
        }
        QuickToggle {
            width: parent.cell
            icon: "preferences-color-symbolic"
            title: "Colors"
            subtitle: (System.colorModes.find(c => c[0] === System.colorMode) ?? ["", ""])[1]
            expandable: true
            expanded: menu.expanded === "colors"
            // the tile steps through the schemes, the arrow lists them
            onToggled: {
                const i = System.colorModes.findIndex(c => c[0] === System.colorMode)
                System.setColorMode(System.colorModes[(i + 1) % System.colorModes.length][0])
            }
            onExpand: menu.expanded = menu.expanded === "colors" ? "" : "colors"
        }
    }

    // ---- colour schemes (the panel's colour tables, MIUI's names)
    Column {
        width: parent.width
        spacing: 4
        visible: menu.expanded === "colors"
        Repeater {
            model: System.colorModes
            Tap {
                required property var modelData
                width: parent.width
                height: 44
                radius: 14
                onClicked: System.setColorMode(modelData[0])
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData[1]
                    color: Theme.fg
                    font.family: Theme.font
                    font.pixelSize: Theme.textSize
                    font.weight: System.colorMode === modelData[0] ? Font.DemiBold : Font.Normal
                }
                SymIcon {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    size: 16
                    visible: System.colorMode === modelData[0]
                    name: "object-select-symbolic"
                }
            }
        }
    }

    // ---- flashlight levels (cezanne-flashlight's: a quarter, half, full)
    Column {
        width: parent.width
        spacing: 4
        visible: menu.expanded === "flash"
        Repeater {
            model: [["Low", 0.25], ["Medium", 0.5], ["High", 1.0]]
            Tap {
                required property var modelData
                width: parent.width
                height: 44
                radius: 14
                onClicked: { ShellState.flashLevel = modelData[1]; System.setFlashlight(true) }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData[0]
                    color: Theme.fg
                    font.family: Theme.font
                    font.pixelSize: Theme.textSize
                    font.weight: ShellState.flashLevel === modelData[1] ? Font.DemiBold : Font.Normal
                }
                SymIcon {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    size: 16
                    visible: ShellState.flashLevel === modelData[1]
                    name: "object-select-symbolic"
                }
            }
        }
    }
}
