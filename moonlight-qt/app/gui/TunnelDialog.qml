import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import "theme"

Basic.Dialog {
    id: dialog

    required property var center
    readonly property var tunnel: center ? center.tunnel : null
    readonly property var centerDefaults: center ? center.centerTunnelDefaults : ({})
    readonly property bool hasCenterDefaults: !!centerDefaults && (centerDefaults.provider === "tailscale" || centerDefaults.provider === "easytier")

    property string activeTab: "tailscale"
    property bool useWebOAuth: true

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min((parent ? parent.width : 680) - 36, 560)
    height: Math.min((parent ? parent.height : 740) - 36, 660)
    title: qsTr("Built-in tunnel")
    modal: true
    standardButtons: Dialog.Close
    palette.window: Theme.surface
    palette.windowText: Theme.text
    palette.buttonText: Theme.text
    padding: 18

    function syncFromManager() {
        if (!tunnel) return
        useWebOAuth = tunnel.tsUseWebOAuth !== false
        hostnameField.text = tunnel.hostname || ""
        tsAutoCheck.checked = tunnel.tsAutoConnect === true
        tsControlField.text = tunnel.tsControlUrl || ""
        tsAuthKeyField.text = tunnel.tsAuthKey || ""
        etAutoCheck.checked = tunnel.etAutoConnect === true
        etNameField.text = tunnel.etNetworkName || ""
        etSecretField.text = tunnel.etNetworkSecret || ""
        etPeersField.text = tunnel.etPeers || "tcp://public.easytier.top:11010"
        etStaticIpField.text = tunnel.etStaticIp || ""
        etCoreField.text = tunnel.etCoreBinary || ""
    }

    onOpened: {
        syncFromManager()
        if (tunnel) tunnel.refreshCapabilities()
    }

    component LabelText: Text {
        color: Theme.text
        font.family: Theme.fontSans
        font.pixelSize: 13
    }

    component InputBox: Basic.TextField {
        id: box
        readOnly: Qt.platform.os === "windows" && !activeFocus
        color: Theme.text
        placeholderTextColor: Theme.textDim
        selectByMouse: true
        padding: 9
        font.family: Theme.fontSans
        font.pixelSize: 13
        background: Rectangle {
            radius: Theme.radiusControl
            color: Theme.surface2
            border.color: box.activeFocus ? Theme.accent : Theme.line
        }
    }

    component ActionBtn: Basic.Button {
        id: btn
        property bool primary: false
        property bool selected: false
        implicitHeight: 35
        leftPadding: 11
        rightPadding: 11
        font.family: Theme.fontSans
        font.pixelSize: 13
        font.weight: Font.DemiBold
        contentItem: Text {
            text: btn.text
            font: btn.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: !btn.enabled ? Theme.textDim : btn.primary ? Theme.accentText : Theme.text
        }
        background: Rectangle {
            radius: Theme.radiusControl
            color: !btn.enabled ? Theme.surface2 : btn.primary
                ? (btn.hovered ? Theme.accentStrong : Theme.accent)
                : btn.hovered || btn.selected ? Theme.accentSoft : Theme.surfaceLayer
            border.color: btn.primary && btn.enabled ? "transparent" : btn.selected ? Theme.accent : Theme.line
            border.width: 1
        }
    }

    contentItem: ScrollView {
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: parent.width
            spacing: 12

            LabelText {
                Layout.fillWidth: true
                color: Theme.textDim
                wrapMode: Text.WordWrap
                text: qsTr("Tailscale / Headscale and EasyTier can run simultaneously in isolated userspace without touching your system VPN or routes. When both tunnels are available, SunAndMon automatically selects and fails over according to your route policy.")
            }

            Rectangle {
                visible: !!tunnel && tunnel.managedBySystemService
                Layout.fillWidth: true
                implicitHeight: svcManagedLabel.implicitHeight + 14
                radius: Theme.radiusControl
                color: Theme.accentSoft
                border.color: Theme.accent
                border.width: 1

                LabelText {
                    id: svcManagedLabel
                    anchors.fill: parent
                    anchors.margins: 7
                    color: Theme.text
                    wrapMode: Text.WordWrap
                    text: qsTr("内置隧道当前由登录前系统服务（Session 0）统一托管，锁屏与重启登录前均可直接远控；此处操作将直接作用于系统服务隧道。")
                }
            }

            // Route priority policy (solves multi-tunnel automatic selection!)
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: routeCol.implicitHeight + 18
                radius: Theme.radiusControl
                color: Theme.surface2
                border.color: Theme.line

                ColumnLayout {
                    id: routeCol
                    anchors.fill: parent
                    anchors.margins: 9
                    spacing: 6

                    LabelText {
                        font.weight: Font.DemiBold
                        text: qsTr("Connection route policy (auto-failover across available routes)")
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 6
                        rowSpacing: 6
                        ActionBtn {
                            Layout.fillWidth: true
                            text: qsTr("Auto (Direct → EasyTier → Tailscale)")
                            selected: !!tunnel && tunnel.routePolicy === "auto"
                            onClicked: if (tunnel) tunnel.routePolicy = "auto"
                        }
                        ActionBtn {
                            Layout.fillWidth: true
                            text: qsTr("Auto (Direct → Tailscale → EasyTier)")
                            selected: !!tunnel && tunnel.routePolicy === "auto_ts"
                            onClicked: if (tunnel) tunnel.routePolicy = "auto_ts"
                        }
                        ActionBtn {
                            Layout.fillWidth: true
                            text: qsTr("Prefer EasyTier tunnel")
                            selected: !!tunnel && tunnel.routePolicy === "prefer_et"
                            onClicked: if (tunnel) tunnel.routePolicy = "prefer_et"
                        }
                        ActionBtn {
                            Layout.fillWidth: true
                            text: qsTr("Prefer Tailscale tunnel")
                            selected: !!tunnel && tunnel.routePolicy === "prefer_ts"
                            onClicked: if (tunnel) tunnel.routePolicy = "prefer_ts"
                        }
                    }
                }
            }

            // Center defaults banner
            Rectangle {
                visible: dialog.hasCenterDefaults
                Layout.fillWidth: true
                implicitHeight: defaultsRow.implicitHeight + 14
                radius: Theme.radiusControl
                color: Theme.accentSoft
                border.color: Theme.accent

                RowLayout {
                    id: defaultsRow
                    anchors.fill: parent
                    anchors.margins: 7
                    spacing: 8
                    LabelText {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Center server provides default %1 tunnel settings.").arg(
                            dialog.centerDefaults.provider === "easytier" ? "EasyTier" : "Tailscale / Headscale")
                    }
                    ActionBtn {
                        text: qsTr("Load defaults")
                        onClicked: {
                            if (tunnel) {
                                tunnel.applyCenterDefaults(dialog.centerDefaults)
                                dialog.activeTab = dialog.centerDefaults.provider
                                dialog.syncFromManager()
                            }
                        }
                    }
                }
            }

            // Shared device hostname
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                LabelText { text: qsTr("Tunnel hostname"); color: Theme.textDim }
                InputBox {
                    id: hostnameField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Auto (sunandmon-<computer>)")
                }
            }

            // Independent provider tabs (both can be connected at the same time!)
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                ActionBtn {
                    Layout.fillWidth: true
                    selected: dialog.activeTab === "tailscale"
                    text: "Tailscale / Headscale" + (tunnel && tunnel.tsState === "connected" ? "  ●" : "")
                    onClicked: dialog.activeTab = "tailscale"
                }
                ActionBtn {
                    Layout.fillWidth: true
                    selected: dialog.activeTab === "easytier"
                    text: "EasyTier" + (tunnel && tunnel.etState === "connected" ? "  ●" : "")
                    onClicked: dialog.activeTab = "easytier"
                }
            }

            // Tailscale / Headscale panel
            ColumnLayout {
                visible: dialog.activeTab === "tailscale"
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: tsStatusCol.implicitHeight + 18
                    radius: Theme.radiusControl
                    color: Theme.surface2
                    border.color: Theme.line
                    ColumnLayout {
                        id: tsStatusCol
                        anchors.fill: parent
                        anchors.margins: 9
                        spacing: 5
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Rectangle {
                                width: 9; height: 9; radius: 5
                                color: !tunnel ? Theme.textDim
                                     : tunnel.tsState === "connected" ? Theme.success
                                     : tunnel.tsState === "needs_auth" || tunnel.tsState === "starting" ? Theme.accent
                                     : tunnel.tsState === "error" ? "#e5534b" : Theme.textDim
                            }
                            LabelText {
                                font.weight: Font.DemiBold
                                text: !tunnel || !tunnel.available ? qsTr("Tunnel binary unavailable")
                                    : tunnel.tsState === "connected" ? qsTr("Connected (Tailscale / Headscale)")
                                    : tunnel.tsState === "needs_auth" ? qsTr("Waiting for browser sign-in (Web OAuth)")
                                    : tunnel.tsState === "starting" ? qsTr("Connecting…")
                                    : tunnel.tsState === "error" ? qsTr("Connection error") : qsTr("Disconnected")
                            }
                            Item { Layout.fillWidth: true }
                            LabelText {
                                visible: !!tunnel && tunnel.tsState === "connected"
                                color: Theme.textDim
                                text: qsTr("%1 peers").arg(tunnel ? tunnel.tsPeersOnline : 0)
                            }
                        }
                        LabelText {
                            visible: !!tunnel && tunnel.tsVirtualIp.length > 0
                            color: Theme.textDim
                            text: qsTr("Virtual IP: %1").arg(tunnel ? tunnel.tsVirtualIp : "")
                        }
                        LabelText {
                            visible: !!tunnel && tunnel.tsError.length > 0
                            color: "#e5534b"
                            text: tunnel ? tunnel.tsError : ""
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                        RowLayout {
                            visible: !!tunnel && tunnel.tsAuthUrl.length > 0
                            Layout.fillWidth: true
                            spacing: 8
                            ActionBtn {
                                primary: true
                                text: qsTr("Open browser to sign in")
                                onClicked: if (tunnel) tunnel.openTailscaleAuthUrl()
                            }
                            LabelText {
                                Layout.fillWidth: true
                                color: Theme.textDim
                                elide: Text.ElideMiddle
                                text: tunnel ? tunnel.tsAuthUrl : ""
                            }
                        }
                    }
                }

                LabelText { font.weight: Font.DemiBold; text: qsTr("Authentication mode") }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    ActionBtn {
                        Layout.fillWidth: true
                        text: qsTr("Web OAuth (browser)")
                        selected: dialog.useWebOAuth
                        onClicked: dialog.useWebOAuth = true
                    }
                    ActionBtn {
                        Layout.fillWidth: true
                        text: qsTr("Manual Auth Key")
                        selected: !dialog.useWebOAuth
                        onClicked: dialog.useWebOAuth = false
                    }
                }

                LabelText { text: qsTr("Control server URL (leave blank for official Tailscale, or enter Headscale URL)"); color: Theme.textDim; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                InputBox {
                    id: tsControlField
                    Layout.fillWidth: true
                    placeholderText: "https://headscale.example.com"
                }

                LabelText { visible: !dialog.useWebOAuth; text: qsTr("Pre-Auth Key"); color: Theme.textDim }
                InputBox {
                    id: tsAuthKeyField
                    visible: !dialog.useWebOAuth
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    placeholderText: "tskey-auth-..."
                }

                HardCheckBox {
                    id: tsAutoCheck
                    text: qsTr("Auto-connect Tailscale / Headscale on launch")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Item { Layout.fillWidth: true }
                    ActionBtn {
                        visible: !!tunnel && tunnel.tsState !== "stopped"
                        text: qsTr("Disconnect Tailscale")
                        onClicked: if (tunnel) tunnel.stopTailscale()
                    }
                    ActionBtn {
                        primary: true
                        text: qsTr("Save & Connect Tailscale")
                        onClicked: if (tunnel) tunnel.saveAndConnectTailscale(
                            tsAutoCheck.checked, hostnameField.text, tsControlField.text, tsAuthKeyField.text, dialog.useWebOAuth)
                    }
                }
            }

            // EasyTier panel
            ColumnLayout {
                visible: dialog.activeTab === "easytier"
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: etStatusCol.implicitHeight + 18
                    radius: Theme.radiusControl
                    color: Theme.surface2
                    border.color: Theme.line
                    ColumnLayout {
                        id: etStatusCol
                        anchors.fill: parent
                        anchors.margins: 9
                        spacing: 5
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Rectangle {
                                width: 9; height: 9; radius: 5
                                color: !tunnel ? Theme.textDim
                                     : tunnel.etState === "connected" ? Theme.success
                                     : tunnel.etState === "starting" ? Theme.accent
                                     : tunnel.etState === "error" ? "#e5534b" : Theme.textDim
                            }
                            LabelText {
                                font.weight: Font.DemiBold
                                text: !tunnel || !tunnel.available ? qsTr("Tunnel binary unavailable")
                                    : tunnel.etState === "connected" ? qsTr("Connected (EasyTier)")
                                    : tunnel.etState === "starting" ? qsTr("Connecting…")
                                    : tunnel.etState === "error" ? qsTr("Connection error") : qsTr("Disconnected")
                            }
                            Item { Layout.fillWidth: true }
                            LabelText {
                                visible: !!tunnel && tunnel.etState === "connected"
                                color: Theme.textDim
                                text: qsTr("%1 peers").arg(tunnel ? tunnel.etPeersOnline : 0)
                            }
                        }
                        LabelText {
                            visible: !!tunnel && tunnel.etVirtualIp.length > 0
                            color: Theme.textDim
                            text: tunnel && tunnel.etExposeIp.length > 0
                                  ? qsTr("Virtual IP: %1  ·  Host tunnel IP: %2").arg(tunnel.etVirtualIp).arg(tunnel.etExposeIp)
                                  : qsTr("Virtual IP: %1").arg(tunnel ? tunnel.etVirtualIp : "")
                            wrapMode: Text.WrapAnywhere
                            Layout.fillWidth: true
                        }
                        LabelText {
                            visible: !!tunnel && tunnel.etError.length > 0
                            color: "#e5534b"
                            text: tunnel ? tunnel.etError : ""
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }

                LabelText {
                    visible: !!tunnel && !tunnel.easytierAvailable
                    color: "#e5534b"
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    text: qsTr("easytier-core / easytier-cli not detected next to the tunnel binary. Specify its path below or use the full SunAndMon package.")
                }

                LabelText { text: qsTr("Network name"); color: Theme.textDim }
                InputBox { id: etNameField; Layout.fillWidth: true; placeholderText: "sunandmon-mesh" }

                LabelText { text: qsTr("Network secret"); color: Theme.textDim }
                InputBox { id: etSecretField; Layout.fillWidth: true; echoMode: TextInput.Password; placeholderText: qsTr("Shared network secret") }

                LabelText { text: qsTr("Peer / relay nodes (comma or space separated)"); color: Theme.textDim }
                InputBox { id: etPeersField; Layout.fillWidth: true; placeholderText: "tcp://public.easytier.top:11010" }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 4
                        LabelText { text: qsTr("Static virtual IP (optional, blank = DHCP)"); color: Theme.textDim }
                        InputBox { id: etStaticIpField; Layout.fillWidth: true; placeholderText: "10.126.126.10" }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 4
                        LabelText { text: qsTr("easytier-core path (optional)"); color: Theme.textDim }
                        InputBox { id: etCoreField; Layout.fillWidth: true; placeholderText: qsTr("Auto-detect") }
                    }
                }

                HardCheckBox {
                    id: etAutoCheck
                    text: qsTr("Auto-connect EasyTier on launch")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Item { Layout.fillWidth: true }
                    ActionBtn {
                        visible: !!tunnel && tunnel.etState !== "stopped"
                        text: qsTr("Disconnect EasyTier")
                        onClicked: if (tunnel) tunnel.stopEasyTier()
                    }
                    ActionBtn {
                        primary: true
                        text: qsTr("Save & Connect EasyTier")
                        onClicked: if (tunnel) tunnel.saveAndConnectEasyTier(
                            etAutoCheck.checked, hostnameField.text, etNameField.text, etSecretField.text,
                            etPeersField.text, etStaticIpField.text, etCoreField.text)
                    }
                }
            }
        }
    }
}
