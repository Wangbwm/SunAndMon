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

    property string selectedProvider: "none"
    property bool useWebOAuth: true

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min((parent ? parent.width : 640) - 36, 520)
    height: Math.min((parent ? parent.height : 700) - 36, 620)
    title: qsTr("Built-in tunnel")
    modal: true
    standardButtons: Dialog.Close
    palette.window: Theme.surface
    palette.windowText: Theme.text
    palette.buttonText: Theme.text
    padding: 18

    function syncFromManager() {
        if (!tunnel) return
        selectedProvider = tunnel.provider || "none"
        useWebOAuth = tunnel.tsUseWebOAuth !== false
        autoConnectCheck.checked = tunnel.autoConnect === true
        hostnameField.text = tunnel.hostname || ""
        tsControlField.text = tunnel.tsControlUrl || ""
        tsAuthKeyField.text = tunnel.tsAuthKey || ""
        etNameField.text = tunnel.etNetworkName || ""
        etSecretField.text = tunnel.etNetworkSecret || ""
        etPeersField.text = tunnel.etPeers || "tcp://public.easytier.top:11010"
        etVirtualIpField.text = tunnel.etVirtualIp || ""
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
        implicitHeight: 36
        leftPadding: 12
        rightPadding: 12
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
                text: qsTr("Runs in isolated userspace without touching your system Tailscale, VPN, or routing table. Configuration and node identity persist across updates.")
            }

            // Live status card
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: statusCol.implicitHeight + 20
                radius: Theme.radiusControl
                color: Theme.surface2
                border.color: Theme.line

                ColumnLayout {
                    id: statusCol
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            width: 9; height: 9; radius: 5
                            color: !tunnel ? Theme.textDim
                                 : tunnel.state === "connected" ? Theme.success
                                 : tunnel.state === "needs_auth" || tunnel.state === "starting" ? Theme.accent
                                 : tunnel.state === "error" ? "#e5534b" : Theme.textDim
                        }
                        LabelText {
                            font.weight: Font.DemiBold
                            text: {
                                if (!tunnel || !tunnel.available) return qsTr("Tunnel binary unavailable")
                                if (tunnel.state === "connected") return qsTr("Connected (%1)").arg(tunnel.provider === "easytier" ? "EasyTier" : "Tailscale / Headscale")
                                if (tunnel.state === "needs_auth") return qsTr("Waiting for browser sign-in (Web OAuth)")
                                if (tunnel.state === "starting") return qsTr("Connecting…")
                                if (tunnel.state === "error") return qsTr("Connection error")
                                return qsTr("Disconnected")
                            }
                        }
                        Item { Layout.fillWidth: true }
                        LabelText {
                            visible: !!tunnel && tunnel.state === "connected"
                            color: Theme.textDim
                            text: qsTr("%1 peers").arg(tunnel ? tunnel.peersOnline : 0)
                        }
                    }

                    LabelText {
                        visible: !!tunnel && tunnel.virtualIp.length > 0
                        color: Theme.textDim
                        text: tunnel && tunnel.exposeIp.length > 0 && tunnel.exposeIp !== tunnel.virtualIp
                              ? qsTr("Virtual IP: %1  ·  Host tunnel IP: %2").arg(tunnel.virtualIp).arg(tunnel.exposeIp)
                              : qsTr("Virtual IP: %1").arg(tunnel ? tunnel.virtualIp : "")
                        wrapMode: Text.WrapAnywhere
                        Layout.fillWidth: true
                    }

                    LabelText {
                        visible: !!tunnel && tunnel.errorText.length > 0
                        color: "#e5534b"
                        text: tunnel ? tunnel.errorText : ""
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    RowLayout {
                        visible: !!tunnel && tunnel.authUrl.length > 0
                        Layout.fillWidth: true
                        spacing: 8
                        ActionBtn {
                            primary: true
                            text: qsTr("Open browser to sign in")
                            onClicked: if (tunnel) tunnel.openAuthUrl()
                        }
                        LabelText {
                            Layout.fillWidth: true
                            color: Theme.textDim
                            elide: Text.ElideMiddle
                            text: tunnel ? tunnel.authUrl : ""
                        }
                    }
                }
            }

            // Center server defaults banner
            Rectangle {
                visible: dialog.hasCenterDefaults
                Layout.fillWidth: true
                implicitHeight: defaultsRow.implicitHeight + 16
                radius: Theme.radiusControl
                color: Theme.accentSoft
                border.color: Theme.accent

                RowLayout {
                    id: defaultsRow
                    anchors.fill: parent
                    anchors.margins: 8
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
                                dialog.syncFromManager()
                            }
                        }
                    }
                }
            }

            // Provider tabs
            LabelText {
                font.weight: Font.DemiBold
                text: qsTr("Tunnel provider")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                ActionBtn {
                    Layout.fillWidth: true
                    text: qsTr("Disabled")
                    selected: dialog.selectedProvider === "none"
                    onClicked: dialog.selectedProvider = "none"
                }
                ActionBtn {
                    Layout.fillWidth: true
                    text: "Tailscale / Headscale"
                    selected: dialog.selectedProvider === "tailscale"
                    onClicked: dialog.selectedProvider = "tailscale"
                }
                ActionBtn {
                    Layout.fillWidth: true
                    text: "EasyTier"
                    selected: dialog.selectedProvider === "easytier"
                    onClicked: dialog.selectedProvider = "easytier"
                }
            }

            // Tailscale / Headscale configuration
            ColumnLayout {
                visible: dialog.selectedProvider === "tailscale"
                Layout.fillWidth: true
                spacing: 8

                LabelText {
                    font.weight: Font.DemiBold
                    text: qsTr("Authentication mode")
                }
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

                LabelText { text: qsTr("Control server URL (leave blank for official Tailscale, or enter Headscale URL)") ; color: Theme.textDim; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                InputBox {
                    id: tsControlField
                    Layout.fillWidth: true
                    placeholderText: "https://headscale.example.com"
                }

                LabelText {
                    visible: !dialog.useWebOAuth
                    text: qsTr("Pre-Auth Key")
                    color: Theme.textDim
                }
                InputBox {
                    id: tsAuthKeyField
                    visible: !dialog.useWebOAuth
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    placeholderText: "tskey-auth-..."
                }
            }

            // EasyTier configuration
            ColumnLayout {
                visible: dialog.selectedProvider === "easytier"
                Layout.fillWidth: true
                spacing: 8

                LabelText {
                    visible: !!tunnel && !tunnel.easytierAvailable
                    color: "#e5534b"
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    text: qsTr("easytier-core / easytier-cli not detected next to the tunnel binary. Specify its path below or use the full SunAndMon package.")
                }

                LabelText { text: qsTr("Network name"); color: Theme.textDim }
                InputBox {
                    id: etNameField
                    Layout.fillWidth: true
                    placeholderText: "sunandmon-mesh"
                }

                LabelText { text: qsTr("Network secret"); color: Theme.textDim }
                InputBox {
                    id: etSecretField
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    placeholderText: qsTr("Shared network secret")
                }

                LabelText { text: qsTr("Peer / relay nodes (comma or space separated)"); color: Theme.textDim }
                InputBox {
                    id: etPeersField
                    Layout.fillWidth: true
                    placeholderText: "tcp://public.easytier.top:11010"
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        LabelText { text: qsTr("Static virtual IP (optional, blank = DHCP)"); color: Theme.textDim }
                        InputBox {
                            id: etVirtualIpField
                            Layout.fillWidth: true
                            placeholderText: "10.126.126.10"
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        LabelText { text: qsTr("easytier-core path (optional)"); color: Theme.textDim }
                        InputBox {
                            id: etCoreField
                            Layout.fillWidth: true
                            placeholderText: qsTr("Auto-detect")
                        }
                    }
                }
            }

            // Shared node options
            ColumnLayout {
                visible: dialog.selectedProvider !== "none"
                Layout.fillWidth: true
                spacing: 8

                LabelText { text: qsTr("Tunnel hostname (optional)"); color: Theme.textDim }
                InputBox {
                    id: hostnameField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Auto (sunandmon-<computer>)")
                }

                HardCheckBox {
                    id: autoConnectCheck
                    text: qsTr("Connect tunnel automatically when SunAndMon starts")
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 8
                Item { Layout.fillWidth: true }
                ActionBtn {
                    visible: !!tunnel && tunnel.state !== "stopped"
                    text: qsTr("Disconnect")
                    onClicked: if (tunnel) tunnel.stopTunnel()
                }
                ActionBtn {
                    primary: true
                    text: dialog.selectedProvider === "none" ? qsTr("Save & Disable") : qsTr("Save & Connect")
                    onClicked: {
                        if (!tunnel) return
                        tunnel.saveAndConnect(
                            dialog.selectedProvider,
                            autoConnectCheck.checked,
                            hostnameField.text,
                            tsControlField.text,
                            tsAuthKeyField.text,
                            dialog.useWebOAuth,
                            etNameField.text,
                            etSecretField.text,
                            etPeersField.text,
                            etVirtualIpField.text,
                            etCoreField.text
                        )
                    }
                }
            }
        }
    }
}
