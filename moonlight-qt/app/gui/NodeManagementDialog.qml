import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic
import QtQuick.Layouts
import "theme"

Basic.Dialog {
    id: dialog
    required property var center
    property bool showDeleted: false
    property string mode: "list"
    property var selectedNode: null

    readonly property var rows: center.adminNodes.filter(node => {
        if (node.deleted !== showDeleted)
            return false
        const query = search.text.trim().toLowerCase()
        const keys = [node.id, titleFor(node), node.reported.name, ...valueFor(node, "addresses", [])]
        return keys.some(key => key && key.toLowerCase().includes(query))
    })

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: parent ? Math.min(720, parent.width - 24) : 720
    height: parent ? Math.min(center.adminAuthenticated ? 590 : 290, parent.height - 24) : 590
    modal: true
    title: qsTr("Manage nodes")
    standardButtons: Dialog.Close
    closePolicy: Popup.CloseOnEscape
    padding: 18
    palette.window: Theme.surface
    palette.windowText: Theme.text
    palette.buttonText: Theme.text

    function valueFor(node, field, fallback) {
        return (node.override && node.override[field]) || node.reported[field] || fallback
    }

    function titleFor(node) {
        return valueFor(node, "name", node.id)
    }

    function signIn() {
        center.adminSignIn(keyField.text)
        keyField.clear()
    }

    function beginEdit(node) {
        selectedNode = node
        const fields = node.override || {}
        nameField.text = fields.name || ""
        addressesField.text = (fields.addresses || []).join("\n")
        portField.text = fields.port ? String(fields.port) : ""
        httpsField.text = fields.httpsPort ? String(fields.httpsPort) : ""
        mode = "edit"
        nameField.forceActiveFocus()
    }

    onOpened: keyField.forceActiveFocus()
    onClosed: {
        keyField.clear()
        search.clear()
        showDeleted = false
        mode = "list"
        selectedNode = null
        center.adminSignOut()
    }

    Connections {
        target: dialog.center
        function onAdminMutationSucceeded() {
            dialog.mode = "list"
            dialog.selectedNode = null
        }
        function onAdminChanged() {
            if (!dialog.center.adminAuthenticated) {
                dialog.mode = "list"
                dialog.selectedNode = null
                if (dialog.visible && !dialog.center.adminBusy)
                    keyField.forceActiveFocus()
            }
        }
        function onChanged() {
            if (dialog.visible && !dialog.center.connected)
                dialog.close()
        }
    }

    component LabelText: Text {
        color: Theme.text
        font.family: Theme.fontSans
        font.pixelSize: 14
        textFormat: Text.PlainText
    }

    component Field: HardTextField {
        readOnly: !activeFocus
    }

    contentItem: ColumnLayout {
        spacing: 12
        LabelText {
            Layout.fillWidth: true
            text: dialog.center.centerIp.length > 0
                  ? qsTr("Connected via IP %1").arg(dialog.center.centerIp) : dialog.center.centerUrl
            color: Theme.textDim
            elide: Text.ElideRight
        }
        LabelText {
            visible: text.length > 0
            text: dialog.center.adminStatus
            Layout.fillWidth: true
            color: Theme.textDim
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            visible: !dialog.center.adminAuthenticated
            Field {
                id: keyField
                Layout.fillWidth: true
                enabled: !dialog.center.adminBusy
                placeholderText: qsTr("Administrator key")
                Accessible.name: placeholderText
                echoMode: TextInput.Password
                onAccepted: if (!dialog.center.adminBusy) dialog.signIn()
            }
            HardButton {
                text: qsTr("Sign in")
                primary: true
                enabled: !dialog.center.adminBusy && keyField.text.length > 0
                onClicked: dialog.signIn()
            }
        }
        RowLayout {
            visible: dialog.center.adminAuthenticated && dialog.mode === "list"
            Layout.fillWidth: true
            HardButton {
                text: qsTr("Registered")
                checkable: true
                checked: !dialog.showDeleted
                primary: checked
                onClicked: dialog.showDeleted = false
            }
            HardButton {
                text: qsTr("Deleted")
                checkable: true
                checked: dialog.showDeleted
                primary: checked
                onClicked: dialog.showDeleted = true
            }
            Field {
                id: search
                Layout.fillWidth: true
                placeholderText: qsTr("Search nodes")
                Accessible.name: placeholderText
            }
            HardButton {
                text: qsTr("Refresh")
                enabled: !dialog.center.adminBusy
                onClicked: dialog.center.refreshAdminNodes()
            }
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: dialog.center.adminAuthenticated && dialog.mode === "list"
            clip: true
            spacing: 8
            model: dialog.rows
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: row
                required property var modelData
                width: list.width - 12
                implicitHeight: details.implicitHeight + 20
                color: Theme.surfaceLayer
                border.color: Theme.line
                radius: Theme.radiusControl
                RowLayout {
                    id: details
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 8
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        LabelText {
                            Layout.fillWidth: true
                            text: dialog.titleFor(row.modelData)
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        LabelText {
                            Layout.fillWidth: true
                            text: row.modelData.id
                            font.family: Theme.fontMono
                            font.pixelSize: 11
                            color: Theme.textDim
                            elide: Text.ElideRight
                        }
                        LabelText {
                            Layout.fillWidth: true
                            visible: !row.modelData.deleted
                            text: {
                                const node = row.modelData
                                const addresses = dialog.valueFor(node, "addresses", []).join(", ")
                                return qsTr("%1  ·  HTTP %2  ·  HTTPS %3")
                                    .arg(addresses).arg(dialog.valueFor(node, "port", "—"))
                                    .arg(dialog.valueFor(node, "httpsPort", "—"))
                            }
                            font.pixelSize: 12
                            color: Theme.textDim
                            elide: Text.ElideRight
                        }
                    }
                    LabelText {
                        visible: !row.modelData.deleted
                        text: row.modelData.online ? qsTr("Online") : qsTr("Offline")
                        color: row.modelData.online ? Theme.accent : Theme.textDim
                    }
                    HardButton {
                        visible: !row.modelData.deleted
                        text: qsTr("Edit")
                        enabled: !dialog.center.adminBusy
                        onClicked: dialog.beginEdit(row.modelData)
                    }
                    HardButton {
                        visible: !row.modelData.deleted
                        text: qsTr("Delete")
                        enabled: !dialog.center.adminBusy
                        onClicked: {
                            dialog.selectedNode = row.modelData
                            dialog.mode = "delete"
                            cancelDelete.forceActiveFocus()
                        }
                    }
                    HardButton {
                        visible: row.modelData.deleted
                        text: qsTr("Allow registration")
                        enabled: !dialog.center.adminBusy
                        onClicked: dialog.center.restoreAdminNode(row.modelData.id)
                    }
                }
            }
            LabelText {
                anchors.centerIn: parent
                visible: list.count === 0
                color: Theme.textDim
                text: dialog.center.adminBusy ? qsTr("Loading…") : qsTr("No matching nodes")
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: dialog.center.adminAuthenticated && dialog.mode === "edit"
            LabelText {
                text: qsTr("Edit node")
                font.weight: Font.DemiBold
            }
            LabelText {
                text: qsTr("Blank fields use the values reported by the host.")
                color: Theme.textDim
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            LabelText { text: qsTr("Display name") }
            Field {
                id: nameField
                Layout.fillWidth: true
                maximumLength: 100
                Accessible.name: qsTr("Display name")
                placeholderText: dialog.selectedNode ? (dialog.selectedNode.reported.name || "") : ""
            }
            LabelText { text: qsTr("IP addresses (one per line)") }
            Basic.TextArea {
                id: addressesField
                Layout.fillWidth: true
                Layout.fillHeight: true
                Accessible.name: qsTr("IP addresses (one per line)")
                color: Theme.text
                readOnly: !activeFocus
                placeholderText: dialog.selectedNode ? (dialog.selectedNode.reported.addresses || []).join("\n") : ""
                background: Rectangle {
                    color: Theme.surfaceLayer
                    border.color: Theme.line
                    radius: Theme.radiusControl
                }
            }
            RowLayout {
                ColumnLayout {
                    Layout.fillWidth: true
                    LabelText { text: qsTr("HTTP port") }
                    Field {
                        id: portField
                        Layout.fillWidth: true
                        Accessible.name: qsTr("HTTP port")
                        placeholderText: dialog.selectedNode ? String(dialog.selectedNode.reported.port || "") : ""
                        inputMethodHints: Qt.ImhDigitsOnly
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    LabelText { text: qsTr("HTTPS port") }
                    Field {
                        id: httpsField
                        Layout.fillWidth: true
                        Accessible.name: qsTr("HTTPS port")
                        placeholderText: dialog.selectedNode ? String(dialog.selectedNode.reported.httpsPort || "") : ""
                        inputMethodHints: Qt.ImhDigitsOnly
                    }
                }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                HardButton {
                    text: qsTr("Cancel")
                    enabled: !dialog.center.adminBusy
                    onClicked: dialog.mode = "list"
                }
                HardButton {
                    text: qsTr("Save")
                    primary: true
                    enabled: !dialog.center.adminBusy && dialog.selectedNode !== null
                    onClicked: dialog.center.editAdminNode(dialog.selectedNode.id, nameField.text,
                        addressesField.text.split(/\s+/).filter(value => value.length > 0),
                        portField.text, httpsField.text)
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: dialog.center.adminAuthenticated && dialog.mode === "delete"
            LabelText {
                Layout.fillWidth: true
                text: dialog.selectedNode ? qsTr("Delete %1?").arg(dialog.titleFor(dialog.selectedNode)) : ""
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }
            LabelText {
                Layout.fillWidth: true
                color: Theme.textDim
                text: qsTr("This node will be removed from the center until registration is allowed again.")
                wrapMode: Text.Wrap
            }
            Item { Layout.fillHeight: true }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                HardButton {
                    id: cancelDelete
                    text: qsTr("Cancel")
                    enabled: !dialog.center.adminBusy
                    onClicked: dialog.mode = "list"
                }
                HardButton {
                    text: qsTr("Delete")
                    enabled: !dialog.center.adminBusy && dialog.selectedNode !== null
                    onClicked: dialog.center.deleteAdminNode(dialog.selectedNode.id)
                }
            }
        }
        Item { visible: !dialog.center.adminAuthenticated; Layout.fillHeight: true }
    }
}
