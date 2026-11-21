import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import "theme"

Rectangle {
    id: dashboard

    required property var center
    property var lanDevices: []
    property bool discoveryEnabled: true
    readonly property bool showingLan: Theme.modern && !showDirectory
    readonly property var deviceSource: showingLan ? lanDevices.filter(device => !device.local) : center.devices
    property string lanSelectionId
    property string centerSelectionId
    property bool showDirectory: true
    property string connectionAction: "desktop"
    property bool showingLocal: false
    property bool startupChangeFailed: false
    property bool elevatedServiceChangeFailed: false
    property url coverSource: "qrc:/res/device-cover.png"
    readonly property string selectedId: showingLan ? lanSelectionId : centerSelectionId
    readonly property var selectedDevice: deviceSource.find(device => device.id === selectedId)
        || deviceSource.find(device => device.online) || deviceSource[0]
    readonly property color ink: Theme.text
    readonly property color muted: Theme.textDim
    readonly property color line: Theme.line
    readonly property color accent: Theme.accent
    readonly property color paper: Theme.ink
    readonly property bool canConnect: !!selectedDevice && selectedDevice.online && (showingLan || !center.busy)
    readonly property bool hostReady: center.hostReady === true
    readonly property bool hostStopping: center.hostStopping === true
    readonly property bool hostStartupTimedOut: center.hostStartupTimedOut === true
    readonly property bool hostHasPortConflict: center.hostPortConflict > 0
    readonly property string hostState: hostStopping ? qsTr("Stopping") : hostReady ? qsTr("Hosting")
        : hostHasPortConflict ? qsTr("Port in use") : hostStartupTimedOut ? qsTr("Startup timed out")
        : center.hostActive ? qsTr("Starting") : qsTr("Off")
    color: Theme.modern ? "transparent" : paper

    signal settingsRequested()
    signal directoryRequested(bool show)
    signal addLanRequested()
    signal lanActionRequested(var device, string action)
    signal lanMenuRequested(var device, Item item, point where)

    function showSource(directory) {
        showDirectory = directory
        showingLocal = false
    }

    function selectDevice(device) {
        if (showingLan) {
            lanSelectionId = device.id
        } else {
            centerSelectionId = device.id
        }
        showingLocal = false
    }

    function showDeviceMenu(device, item, point) {
        if (showingLan) {
            lanMenuRequested(device, item, point)
        } else {
            directoryMenu.device = device
            const mapped = item.mapToItem(dashboard, point)
            directoryMenu.popup(mapped.x, mapped.y)
        }
    }

    function connectSelected(action) {
        if (!canConnect) {
            return
        }
        if (showingLan) {
            lanActionRequested(selectedDevice, action)
        } else {
            connectionAction = action
            center.connectDevice(selectedDevice.id)
        }
    }

    component Copy: Text {
        color: dashboard.ink
        font.family: Theme.fontSans
        font.pixelSize: 14
    }

    component Action: Basic.Button {
        id: action
        property bool primary: false
        property bool selected: false
        implicitHeight: 40
        leftPadding: 14
        rightPadding: 14
        font.family: Theme.fontSans
        font.pixelSize: 13
        font.weight: Font.DemiBold
        Keys.onReturnPressed: if (enabled) clicked()
        Keys.onEnterPressed: if (enabled) clicked()
        contentItem: Copy {
            text: action.text
            font: action.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: !action.enabled ? dashboard.muted : action.primary ? Theme.accentText : dashboard.ink
        }
        background: Rectangle {
            radius: Theme.radiusControl
            color: !action.enabled ? Theme.surface2 : action.primary
                ? (action.hovered ? Theme.accentStrong : dashboard.accent)
                : action.hovered || action.selected ? Theme.accentSoft : Theme.surfaceLayer
            border.color: action.visualFocus ? dashboard.accent : action.primary && action.enabled ? "transparent" : dashboard.line
            border.width: action.visualFocus ? 2 : 1
        }
    }

    component Input: Basic.TextField {
        id: input
        // Qt checks the system clipboard on every change while a field is writable.
        readOnly: Qt.platform.os === "windows" && !activeFocus
        color: dashboard.ink
        placeholderTextColor: dashboard.muted
        selectByMouse: true
        padding: 10
        background: Rectangle {
            radius: Theme.radiusControl
            color: Theme.surface
            border.color: input.activeFocus ? dashboard.accent : dashboard.line
        }
    }

    component Monitor: Item {
        id: icon
        property color tint: dashboard.accent
        width: 23
        height: 23
        Rectangle { x: 1; y: 2; width: 21; height: 14; radius: 3; color: "transparent"; border.color: icon.tint }
        Rectangle { x: 10.5; y: 16; width: 2; height: 4; color: icon.tint }
        Rectangle { x: 7; y: 20; width: 9; height: 2; radius: 1; color: icon.tint }
    }

    component RailButton: Basic.ItemDelegate {
        id: entry
        property string secondary
        property bool current
        property bool showDot
        property bool online
        property bool showMonitor: true
        property color secondaryColor: dashboard.muted
        implicitHeight: 42
        padding: 10
        Keys.onReturnPressed: if (enabled) clicked()
        Keys.onEnterPressed: if (enabled) clicked()
        contentItem: RowLayout {
            spacing: 12
            Monitor {
                visible: entry.showMonitor
                tint: entry.current ? dashboard.accent : dashboard.muted
            }
            Copy {
                Layout.fillWidth: true
                text: entry.text
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.weight: entry.current ? Font.DemiBold : Font.Normal
            }
            Copy { visible: text.length > 0; text: entry.secondary; font.pixelSize: 12; color: entry.secondaryColor }
            Rectangle {
                visible: entry.showDot
                Layout.preferredWidth: 7
                Layout.preferredHeight: 7
                radius: 4
                color: entry.online ? Theme.success : Theme.lineStrong
            }
        }
        background: Rectangle {
            radius: Theme.radiusControl
            color: entry.current ? Theme.accentSoft : entry.hovered ? Theme.surface2 : "transparent"
            border.color: entry.visualFocus ? dashboard.accent : "transparent"
            Rectangle {
                visible: entry.current
                anchors.verticalCenter: parent.verticalCenter
                width: 3; height: 20; radius: 1; color: dashboard.accent
            }
        }
    }

    component QuickAction: Basic.ItemDelegate {
        id: tile
        implicitHeight: 46
        leftPadding: 12
        rightPadding: 12
        spacing: 8
        font.family: Theme.fontSans
        font.pixelSize: 13
        font.weight: Font.DemiBold
        icon.width: 19
        icon.height: 19
        icon.color: tile.enabled ? Theme.accent : Theme.textFaint
        palette.text: tile.enabled ? Theme.text : Theme.textFaint
        Keys.onReturnPressed: if (enabled) clicked()
        Keys.onEnterPressed: if (enabled) clicked()
        background: Rectangle {
            radius: Theme.radiusControl
            color: tile.hovered && tile.enabled ? Theme.accentSoft : "transparent"
            border.width: tile.visualFocus ? 2 : 0
            border.color: Theme.accent
        }
    }

    RowLayout {
        visible: !Theme.modern && !dashboard.showDirectory
        anchors.fill: parent
        anchors.margins: 12
        Action {
            text: qsTr("My devices")
            onClicked: dashboard.directoryRequested(true)
        }
        Copy {
            Layout.fillWidth: true
            text: qsTr("Local network")
            color: dashboard.muted
        }
    }

    RowLayout {
        visible: Theme.modern || dashboard.showDirectory
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: dashboard.width >= 760 ? Theme.railWidth : Math.max(130, dashboard.width * 0.25)
            Layout.fillHeight: true
            color: Theme.railSurface
            Rectangle { anchors.right: parent.right; height: parent.height; width: 1; color: dashboard.line }
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                Copy {
                    Layout.topMargin: 8
                    Layout.leftMargin: 8
                    Layout.bottomMargin: 4
                    text: qsTr("My devices")
                    font.pixelSize: 15
                    font.weight: Font.Bold
                }
                RowLayout {
                    visible: Theme.modern
                    Layout.fillWidth: true
                    spacing: 4
                    Action {
                        Layout.fillWidth: true
                        text: qsTr("Center")
                        selected: dashboard.showDirectory && !dashboard.showingLocal
                        onClicked: {
                            if (!dashboard.showDirectory) {
                                dashboard.directoryRequested(true)
                            } else {
                                dashboard.showSource(true)
                            }
                        }
                    }
                    Action {
                        Layout.fillWidth: true
                        text: qsTr("Local network")
                        selected: dashboard.showingLan && !dashboard.showingLocal
                        onClicked: {
                            if (dashboard.showDirectory) {
                                dashboard.directoryRequested(false)
                            } else {
                                dashboard.showSource(false)
                            }
                        }
                    }
                }
                Input {
                    id: railSearch
                    visible: dashboard.deviceSource.length > 6 || text.length > 0
                    Layout.fillWidth: true
                    placeholderText: qsTr("Search devices")
                    font.pixelSize: 13
                    padding: 7
                }
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 0
                    clip: true
                    readonly property var filteredDevices: {
                        const query = railSearch.text.trim().toLowerCase()
                        if (query.length === 0) {
                            return dashboard.deviceSource
                        }
                        return dashboard.deviceSource.filter(device =>
                            (device.name || "").toLowerCase().includes(query) ||
                            (device.id || "").toLowerCase().includes(query)
                        )
                    }
                    ListView {
                        id: devices
                        anchors.fill: parent
                        spacing: 3
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        model: parent.filteredDevices
                        ScrollBar.vertical: ScrollBar {
                            policy: devices.contentHeight > devices.height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
                        }
                        delegate: RailButton {
                            id: deviceRow
                            required property var modelData
                            width: Math.max(0, ListView.view.width - (devices.contentHeight > devices.height ? 12 : 0))
                            text: modelData.name || qsTr("Unnamed device")
                            current: !dashboard.showingLocal && !!dashboard.selectedDevice && dashboard.selectedDevice.id === modelData.id
                            showDot: true
                            online: modelData.online === true
                            Accessible.description: online ? qsTr("Online") : qsTr("Offline")
                            onClicked: dashboard.selectDevice(modelData)
                            onPressAndHold: dashboard.showDeviceMenu(modelData, this, Qt.point(width / 2, height / 2))
                            Keys.onMenuPressed: dashboard.showDeviceMenu(modelData, this, Qt.point(width / 2, height))
                            Keys.onDeletePressed: dashboard.showDeviceMenu(modelData, this, Qt.point(width / 2, height))
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.RightButton
                                onClicked: function(mouse) {
                                    dashboard.showDeviceMenu(deviceRow.modelData, deviceRow, Qt.point(mouse.x, mouse.y))
                                }
                            }
                        }
                    }
                    Copy {
                        visible: devices.count === 0
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 7
                        text: railSearch.text.trim().length > 0 ? qsTr("No matching devices")
                            : dashboard.showingLan ? qsTr("No devices discovered")
                            : dashboard.center.connected ? qsTr("No registered devices") : qsTr("Sign in to see your devices")
                        font.pixelSize: 13
                        color: dashboard.muted
                        wrapMode: Text.WordWrap
                    }
                }
                Rectangle {
                    Layout.fillWidth: true; Layout.topMargin: 4; Layout.bottomMargin: 4
                    height: 1; color: dashboard.line
                }
                RailButton {
                    Layout.fillWidth: true
                    text: qsTr("Host this computer")
                    current: dashboard.showingLocal
                    secondary: width >= 180 ? dashboard.hostState : ""
                    secondaryColor: dashboard.hostReady ? Theme.success : dashboard.muted
                    showDot: secondary.length === 0
                    online: dashboard.hostReady
                    Accessible.description: dashboard.hostState
                    onClicked: dashboard.showingLocal = true
                }
                RailButton {
                    visible: !Theme.modern
                    Layout.fillWidth: true
                    text: qsTr("Local network")
                    onClicked: dashboard.directoryRequested(false)
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: dashboard.line }
                RailButton {
                    Layout.fillWidth: true
                    text: qsTr("Built-in tunnel")
                    secondary: dashboard.center && dashboard.center.tunnel ? dashboard.center.tunnel.summaryBadge : ""
                    secondaryColor: {
                        const t = dashboard.center ? dashboard.center.tunnel : null
                        if (t && t.state === "connected") return Theme.success
                        if (t && t.state === "error") return "#e5534b"
                        return dashboard.muted
                    }
                    showMonitor: false
                    onClicked: tunnelDialog.open()
                }
                RailButton {
                    Layout.fillWidth: true
                    text: qsTr("Center server")
                    secondary: dashboard.center.connected ? qsTr("Connected") : ""
                    secondaryColor: Theme.success
                    showMonitor: false
                    onClicked: centerDialog.open()
                }
            }
        }

        ScrollView {
            id: details
            Layout.fillHeight: true
            Layout.fillWidth: true
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: Math.min(details.availableWidth, dashboard.showingLocal ? 790 : Theme.contentWidth)
                x: Math.max(0, (details.availableWidth - width) / 2)
                spacing: 16
                readonly property int margin: width >= 630 ? 32 : 16

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: parent.margin
                    Layout.rightMargin: parent.margin
                    Layout.topMargin: 28
                    spacing: 12
                    Copy {
                        Layout.maximumWidth: Math.max(72, parent.width - (deviceBadge.visible ? deviceBadge.implicitWidth : 0)
                            - (refreshButton.visible ? refreshButton.implicitWidth : 0) - 36)
                        textFormat: Text.PlainText
                        text: dashboard.showingLocal ? qsTr("Host this computer")
                            : dashboard.selectedDevice ? dashboard.selectedDevice.name
                            : dashboard.showingLan ? qsTr("Local network") : qsTr("Remote devices")
                        elide: Text.ElideRight
                        font.pixelSize: 30
                        font.weight: Font.Bold
                    }
                    Rectangle {
                        id: deviceBadge
                        visible: !dashboard.showingLocal && !!dashboard.selectedDevice
                        implicitWidth: badge.implicitWidth + 18
                        implicitHeight: 27
                        radius: height / 2
                        color: dashboard.selectedDevice && dashboard.selectedDevice.online ? Theme.successSurface : Theme.surface2
                        Copy {
                            id: badge
                            anchors.centerIn: parent
                            text: dashboard.selectedDevice && dashboard.selectedDevice.online
                                  ? (dashboard.showingLan ? qsTr("Online") : qsTr("Online via center")) : qsTr("Offline")
                            color: dashboard.selectedDevice && dashboard.selectedDevice.online ? Theme.success : dashboard.muted
                            font.weight: Font.DemiBold
                            font.pixelSize: 12
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Action {
                        id: refreshButton
                        visible: !dashboard.showingLocal && (dashboard.showingLan || dashboard.center.connected)
                        text: dashboard.showingLan ? qsTr("Add PC manually") : qsTr("Refresh")
                        enabled: dashboard.showingLan || !dashboard.center.busy
                        onClicked: {
                            if (dashboard.showingLan) {
                                dashboard.addLanRequested()
                            } else {
                                dashboard.center.refresh()
                            }
                        }
                    }
                }

                Rectangle {
                    id: launchCard
                    visible: !dashboard.showingLocal && !!dashboard.selectedDevice
                    Layout.fillWidth: true
                    Layout.leftMargin: parent.margin
                    Layout.rightMargin: parent.margin
                    implicitHeight: launchLayout.implicitHeight + 40
                    radius: Theme.radiusCard
                    border.color: dashboard.line
                    color: Theme.glassSurface

                    RowLayout {
                        id: launchLayout
                        anchors.fill: parent
                        anchors.margins: 20
                        spacing: 18

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            Rectangle {
                                Layout.preferredWidth: 42
                                Layout.preferredHeight: 42
                                radius: Theme.radiusControl
                                color: Theme.accentSoft
                                Monitor { anchors.centerIn: parent }
                            }
                            Copy {
                                Layout.fillWidth: true
                                text: qsTr("Remote desktop")
                                wrapMode: Text.WordWrap
                                font.pixelSize: 22
                                font.weight: Font.Bold
                            }
                            Copy {
                                visible: dashboard.showingLan && text.length > 0
                                Layout.fillWidth: true
                                text: dashboard.selectedDevice ? dashboard.selectedDevice.address || "" : ""
                                color: Theme.textDim
                                elide: Text.ElideRight
                            }
                            Action {
                                text: dashboard.showingLan && dashboard.selectedDevice && !dashboard.selectedDevice.paired
                                      ? qsTr("Pair device") : qsTr("Connect desktop")
                                primary: true
                                enabled: dashboard.canConnect
                                onClicked: dashboard.connectSelected("desktop")
                            }
                        }

                        Image {
                            visible: launchCard.width >= 460
                            Layout.preferredWidth: Math.min(480, launchCard.width * 0.57)
                            Layout.preferredHeight: 162
                            Layout.fillHeight: true
                            source: dashboard.coverSource
                            asynchronous: true
                            fillMode: Image.PreserveAspectCrop
                            clip: true
                        }
                    }
                }

                Rectangle {
                    visible: dashboard.showingLocal
                    Layout.fillWidth: true
                    Layout.leftMargin: parent.margin
                    Layout.rightMargin: parent.margin
                    implicitHeight: localHostCard.implicitHeight + 44
                    radius: Theme.radiusCard
                    color: Theme.glassSurface
                    border.color: dashboard.line

                    GridLayout {
                        id: localHostCard
                        anchors.fill: parent
                        anchors.margins: 22
                        columns: width >= 440 ? 2 : 1
                        columnSpacing: 16
                        rowSpacing: 14

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 14

                            Rectangle {
                                Layout.preferredWidth: 48
                                Layout.preferredHeight: 48
                                radius: Theme.radiusControl
                                color: Theme.accentSoft
                                Monitor { anchors.centerIn: parent }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 5
                                Copy {
                                    Layout.fillWidth: true
                                    text: dashboard.center.computerName || qsTr("This computer")
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    font.pixelSize: 20
                                    font.weight: Font.Bold
                                }
                                RowLayout {
                                    spacing: 6
                                    Rectangle {
                                        Layout.preferredWidth: 7
                                        Layout.preferredHeight: 7
                                        radius: width / 2
                                        color: dashboard.hostReady ? Theme.success
                                            : dashboard.center.hostActive ? Theme.accent : Theme.lineStrong
                                    }
                                    Copy {
                                        text: dashboard.hostState
                                        font.pixelSize: 13
                                        font.weight: Font.DemiBold
                                        color: dashboard.hostReady ? Theme.success : dashboard.muted
                                    }
                                }
                            }
                        }
                        Action {
                            visible: dashboard.center.hostAvailable && !dashboard.hostHasPortConflict
                            enabled: !dashboard.center.busy && !dashboard.hostStopping
                            primary: !dashboard.center.hostActive
                            text: dashboard.center.hostActive ? qsTr("Disable hosting") : qsTr("Enable hosting")
                            onClicked: dashboard.center.hostActive ? dashboard.center.stopHost() : dashboard.center.startHost(publishedIp.text)
                        }
                        Copy {
                            Layout.columnSpan: localHostCard.columns
                            Layout.fillWidth: true
                            visible: !dashboard.center.hostActive || dashboard.hostReady || dashboard.hostStartupTimedOut
                            text: dashboard.center.hostStatus || (dashboard.center.hostAvailable
                                ? qsTr("Host disabled") : qsTr("Hosting is available in the Windows bundle."))
                            wrapMode: Text.WordWrap
                            color: dashboard.muted
                        }
                        Flow {
                            Layout.columnSpan: localHostCard.columns
                            Layout.fillWidth: true
                            visible: dashboard.hostHasPortConflict
                            spacing: 8

                            Action {
                                text: qsTr("Use available ports")
                                primary: true
                                onClicked: dashboard.center.startHostOnAlternatePorts()
                            }
                            Action {
                                visible: dashboard.center.canStopConflictingHost === true
                                text: qsTr("Stop previous Sunshine and retry")
                                onClicked: dashboard.center.stopConflictingHostAndRetry()
                            }
                        }
                        Copy {
                            Layout.columnSpan: localHostCard.columns
                            Layout.fillWidth: true
                            visible: publishedIp.visible
                            text: qsTr("Advertised IP")
                            color: dashboard.muted
                        }
                        Input {
                            id: publishedIp
                            Layout.columnSpan: localHostCard.columns
                            Layout.fillWidth: true
                            Layout.maximumWidth: 360
                            visible: dashboard.center.hostAvailable && dashboard.center.connected
                            readOnly: dashboard.center.hostActive || (Qt.platform.os === "windows" && !activeFocus)
                            text: dashboard.center.advertisedAddress
                            placeholderText: qsTr("Advertised IP (automatic)")
                            Accessible.name: qsTr("Advertised IP")
                        }
                    }
                }

                Rectangle {
                    visible: dashboard.showingLocal
                    Layout.fillWidth: true
                    Layout.leftMargin: parent.margin
                    Layout.rightMargin: parent.margin
                    Layout.bottomMargin: parent.margin
                    implicitHeight: localSettings.implicitHeight + 16
                    radius: Theme.radiusCard
                    color: Theme.glassSurface
                    border.color: dashboard.line

                    ColumnLayout {
                        id: localSettings
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 0

                        QuickAction {
                            Layout.fillWidth: true
                            visible: dashboard.center.hostAvailable
                            enabled: dashboard.hostReady && dashboard.center.hostSettingsUrl.toString().length > 0
                            text: qsTr("Host settings")
                            icon.source: "qrc:/res/fluent/cat-host.svg"
                            onClicked: Qt.openUrlExternally(dashboard.center.hostSettingsUrl)
                        }
                        QuickAction {
                            Layout.fillWidth: true
                            text: qsTr("Streaming settings")
                            icon.source: "qrc:/res/fluent/tb-settings.svg"
                            onClicked: dashboard.settingsRequested()
                        }
                        Rectangle {
                            visible: dashboard.center.hostAvailable
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            Layout.rightMargin: 12
                            implicitHeight: 1
                            color: dashboard.line
                        }
                        RowLayout {
                            visible: dashboard.center.hostAvailable
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            Layout.rightMargin: 12
                            Layout.topMargin: 10
                            Layout.bottomMargin: 7
                            spacing: 12
                            Copy {
                                Layout.fillWidth: true
                                text: Qt.platform.os === "linux" ? qsTr("Start at sign-in") : qsTr("Start at Windows sign-in")
                                wrapMode: Text.WordWrap
                            }
                            HardSwitch {
                                id: startupSwitch
                                Accessible.name: Qt.platform.os === "linux" ? qsTr("Start at sign-in") : qsTr("Start at Windows sign-in")
                                checked: dashboard.center.startAtLogin
                                onToggled: {
                                    dashboard.startupChangeFailed = !dashboard.center.setStartAtLogin(checked)
                                    checked = Qt.binding(() => dashboard.center.startAtLogin)
                                }
                            }
                        }
                        Copy {
                            visible: dashboard.startupChangeFailed
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            Layout.rightMargin: 12
                            Layout.bottomMargin: 7
                            text: Qt.platform.os === "linux" ? qsTr("Could not change login startup.") : qsTr("Could not change Windows startup.")
                            wrapMode: Text.WordWrap
                            color: Theme.danger
                        }
                        Rectangle {
                            visible: dashboard.center.hostServiceAvailable
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            Layout.rightMargin: 12
                            implicitHeight: 1
                            color: dashboard.line
                        }
                        RowLayout {
                            visible: dashboard.center.hostServiceAvailable
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            Layout.rightMargin: 12
                            Layout.topMargin: 10
                            Layout.bottomMargin: 7
                            spacing: 12
                            Copy {
                                Layout.fillWidth: true
                                text: qsTr("Allow remote UAC control (system service)")
                                wrapMode: Text.WordWrap
                            }
                            HardSwitch {
                                id: elevatedServiceSwitch
                                Accessible.name: qsTr("Allow remote UAC control (system service)")
                                checked: dashboard.center.elevatedHostService
                                onToggled: {
                                    dashboard.elevatedServiceChangeFailed = !dashboard.center.setElevatedHostService(checked)
                                    checked = Qt.binding(() => dashboard.center.elevatedHostService)
                                }
                            }
                        }
                        Copy {
                            visible: dashboard.elevatedServiceChangeFailed
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            Layout.rightMargin: 12
                            Layout.bottomMargin: 7
                            text: qsTr("Could not change the elevated host service.")
                            wrapMode: Text.WordWrap
                            color: Theme.danger
                        }
                    }
                }

                Rectangle {
                    visible: !dashboard.showingLocal && !dashboard.selectedDevice
                    Layout.fillWidth: true
                    Layout.leftMargin: parent.margin
                    Layout.rightMargin: parent.margin
                    implicitHeight: emptyCard.implicitHeight + 40
                    radius: Theme.radiusCard
                    color: Theme.glassSurface
                    border.color: dashboard.line

                    ColumnLayout {
                        id: emptyCard
                        anchors.fill: parent
                        anchors.margins: 20
                        spacing: 12
                        Copy {
                            Layout.fillWidth: true
                            text: dashboard.showingLan
                                ? (dashboard.discoveryEnabled ? qsTr("Searching for compatible hosts on your local network...")
                                                               : qsTr("Automatic PC discovery is disabled. Add your PC manually."))
                                : dashboard.center.connected ? qsTr("Enable hosting on another device signed into this center.")
                                : qsTr("Sign in to view devices registered with your center.")
                            wrapMode: Text.WordWrap
                            color: dashboard.muted
                        }
                        Action {
                            visible: !dashboard.showingLan && !dashboard.center.connected
                            text: qsTr("Sign in")
                            primary: true
                            onClicked: centerDialog.open()
                        }
                    }
                }

                Rectangle {
                    Layout.leftMargin: parent.margin
                    Layout.rightMargin: parent.margin
                    Layout.fillWidth: true
                    implicitHeight: statusText.implicitHeight + 22
                    visible: !dashboard.showingLocal && !dashboard.showingLan && statusText.text.length > 0
                    radius: Theme.radiusControl
                    color: Theme.surface2
                    Copy {
                        id: statusText
                        anchors.fill: parent
                        anchors.margins: 11
                        text: dashboard.center.centerStatus
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: dashboard.muted
                        Accessible.ignored: !visible
                    }
                }
                Rectangle {
                    visible: !dashboard.showingLocal
                    Layout.fillWidth: true
                    Layout.leftMargin: parent.margin
                    Layout.rightMargin: parent.margin
                    Layout.bottomMargin: parent.margin
                    implicitHeight: deviceActions.implicitHeight + 14
                    radius: Theme.radiusCard
                    color: Theme.glassSurface
                    border.color: dashboard.line

                    GridLayout {
                        id: deviceActions
                        anchors.fill: parent
                        anchors.margins: 7
                        columns: width >= 670 ? 4 : width >= 315 ? 2 : 1
                        columnSpacing: 4
                        rowSpacing: 3

                        QuickAction {
                            Layout.fillWidth: true
                            visible: !dashboard.showingLocal && !!dashboard.selectedDevice
                            text: qsTr("App list")
                            icon.source: "qrc:/res/fluent/cat-ui.svg"
                            enabled: dashboard.canConnect
                            onClicked: dashboard.connectSelected("apps")
                        }
                        QuickAction {
                            Layout.fillWidth: true
                            visible: !dashboard.showingLocal && !!dashboard.selectedDevice
                            text: qsTr("Displays and virtual screen")
                            icon.source: "qrc:/res/fluent/tb-display.svg"
                            enabled: dashboard.canConnect
                            onClicked: dashboard.connectSelected("displays")
                        }
                        QuickAction {
                            Layout.fillWidth: true
                            visible: !dashboard.showingLocal && !!dashboard.selectedDevice
                            text: qsTr("Device options")
                            icon.source: "qrc:/res/fluent/cat-host.svg"
                            enabled: !!dashboard.selectedDevice
                            onClicked: {
                                if (dashboard.showingLan || !dashboard.canConnect) {
                                    dashboard.showDeviceMenu(dashboard.selectedDevice, this, Qt.point(width / 2, height))
                                } else {
                                    dashboard.connectSelected("options")
                                }
                            }
                        }
                        QuickAction {
                            Layout.fillWidth: true
                            text: qsTr("Streaming settings")
                            icon.source: "qrc:/res/fluent/tb-settings.svg"
                            onClicked: dashboard.settingsRequested()
                        }
                    }
                }
            }
        }
    }

    NavigableMenu {
        id: directoryMenu
        parent: dashboard
        property var device: null
        NavigableMenuItem {
            text: qsTr("Device options")
            visible: !!directoryMenu.device && directoryMenu.device.online
            enabled: !dashboard.center.busy
            onTriggered: {
                dashboard.selectDevice(directoryMenu.device)
                dashboard.connectSelected("options")
            }
        }
        NavigableMenuItem {
            text: qsTr("Remove from my list")
            onTriggered: if (directoryMenu.device) dashboard.center.hideDevice(directoryMenu.device.id)
        }
    }

    Basic.Dialog {
        id: centerDialog
        property bool signingIn
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(dashboard.width - 30, 430)
        title: qsTr("Center server")
        modal: true
        standardButtons: Dialog.Close
        palette.window: Theme.surface
        palette.windowText: dashboard.ink
        palette.buttonText: dashboard.ink
        padding: 18
        onOpened: if (!dashboard.center.connected) addressField.forceActiveFocus()
        onClosed: {
            keyField.clear()
            signingIn = false
        }

        contentItem: ColumnLayout {
            spacing: 10
            Input {
                id: addressField
                Layout.fillWidth: true
                visible: !dashboard.center.connected
                enabled: !dashboard.center.busy
                text: dashboard.center.centerUrl
                placeholderText: "https://center.example.com"
                Accessible.name: qsTr("Center URL")
            }
            Input {
                id: directIpField
                Layout.fillWidth: true
                visible: !dashboard.center.connected
                enabled: addressField.enabled
                text: dashboard.center.centerIp
                placeholderText: qsTr("Direct IP (optional)")
                Accessible.name: placeholderText
            }
            Input {
                id: keyField
                Layout.fillWidth: true
                visible: !dashboard.center.connected
                enabled: addressField.enabled
                echoMode: TextInput.Password
                placeholderText: qsTr("Access key")
                Accessible.name: placeholderText
                onAccepted: if (signInButton.enabled) signInButton.clicked()
            }
            Copy {
                visible: dashboard.center.connected
                Layout.fillWidth: true
                text: dashboard.center.centerUrl
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                color: dashboard.muted
            }
            Copy {
                visible: dashboard.center.connected && dashboard.center.centerIp.length > 0
                Layout.fillWidth: true
                text: qsTr("Connected via IP %1").arg(dashboard.center.centerIp)
                textFormat: Text.PlainText
                color: dashboard.muted
                wrapMode: Text.WrapAnywhere
            }
            Copy {
                visible: text.length > 0
                Layout.fillWidth: true
                text: dashboard.center.centerStatus
                wrapMode: Text.WordWrap
                color: dashboard.muted
            }
            Action {
                visible: dashboard.center.connected
                text: qsTr("Manage nodes")
                onClicked: {
                    centerDialog.close()
                    nodeManagement.open()
                }
            }
            Action {
                visible: dashboard.center.connected && dashboard.center.hiddenDeviceCount > 0
                text: qsTr("Restore hidden devices (%1)").arg(dashboard.center.hiddenDeviceCount)
                onClicked: dashboard.center.restoreHiddenDevices()
            }
            Action {
                id: signInButton
                text: dashboard.center.connected ? qsTr("Sign out") : qsTr("Sign in")
                primary: !dashboard.center.connected
                enabled: !dashboard.center.busy
                onClicked: {
                    if (dashboard.center.connected) {
                        dashboard.center.signOut()
                    } else {
                        centerDialog.signingIn = true
                        dashboard.center.signIn(addressField.text, keyField.text, directIpField.text)
                        keyField.clear()
                    }
                }
            }
        }
    }

    NodeManagementDialog {
        id: nodeManagement
        center: dashboard.center
    }

    TunnelDialog {
        id: tunnelDialog
        center: dashboard.center
    }

    Connections {
        target: dashboard.center
        function onChanged() {
            if (centerDialog.visible && centerDialog.signingIn && !dashboard.center.busy) {
                centerDialog.signingIn = false
                if (dashboard.center.connected)
                    centerDialog.close()
            }
        }
    }
}
