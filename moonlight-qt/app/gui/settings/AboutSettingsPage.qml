pragma ComponentBehavior: Bound
import QtQuick 2.9
import QtQuick.Controls
import Qt.labs.platform 1.1 as PlatformDialogs
import "."
import "../theme"
import AutoUpdateChecker 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0

// “关于”展示版本信息、热更新控制、项目入口与法律声明。
// 更新请求与热更新安装统一交给 AutoUpdateChecker。
Column {
    id: aboutPage

    signal scrollToEndRequested()

    width: parent ? parent.width : 0
    spacing: Theme.spaceLg

    function isSimplifiedChineseLocale() {
        var localeName = Qt.locale().name.replace("-", "_").toLowerCase()
        return localeName === "zh_cn" || localeName.indexOf("zh_hans") === 0
    }

    readonly property bool useMainlandLinks:
        StreamingPreferences.language === StreamingPreferences.LANG_ZH_CN ||
        (StreamingPreferences.language === StreamingPreferences.LANG_AUTO &&
         isSimplifiedChineseLocale())
    readonly property string paragraphIndent: "\u00A0\u00A0\u00A0\u00A0"

    function zh(zhText, enText) {
        return useMainlandLinks ? zhText : enText
    }

    readonly property string sunAndMonRepoUrl: "https://github.com/Wangbwm/SunAndMon"
    readonly property string sunAndMonReleasesUrl: "https://github.com/Wangbwm/SunAndMon/releases"
    readonly property string issueUrl: SystemProperties.isSunAndMon
        ? "https://github.com/Wangbwm/SunAndMon/issues/new/choose"
        : "https://github.com/qiin2333/moonlight-qt/issues/new/choose"
    readonly property string qqUrl: "https://qm.qq.com/q/3tWBFVNZ"
    readonly property string officialSiteUrl: SystemProperties.isSunAndMon
        ? sunAndMonRepoUrl
        : (useMainlandLinks ? "https://www.alkaidlab.cn/" : "https://www.alkaidlab.com/")
    readonly property string bilibiliUrl: "https://space.bilibili.com/3690974838524514"
    readonly property string licenseUrl: SystemProperties.isSunAndMon
        ? "https://github.com/Wangbwm/SunAndMon/blob/main/LICENSE"
        : "https://github.com/AlkaidLab/foundation-sunshine/blob/master/LICENSE"
    readonly property string noticeUrl: SystemProperties.isSunAndMon
        ? "https://github.com/Wangbwm/SunAndMon/blob/main/NOTICE"
        : "https://github.com/AlkaidLab/foundation-sunshine/blob/master/NOTICE"
    readonly property string usbipdcppUrl: "https://github.com/yunsmall/usbipdcpp"
    property bool updateCheckRequested: false
    property bool updateCheckInProgress: false
    property string pendingUpdateVersion: AutoUpdateChecker.latestVersion
    property string updateStatus: AutoUpdateChecker.updateAvailable
        ? qsTr("Update available: Version %1").arg(AutoUpdateChecker.latestVersion)
        : ""

    function openExternal(url) {
        if (!Qt.openUrlExternally(url)) {
            ToolTip.show(qsTr("No external browser is available."), 3500)
        }
    }

    function revealLegalNotice() {
        Qt.callLater(function() { aboutPage.scrollToEndRequested() })
    }

    function requestUpdateCheck() {
        if (updateCheckInProgress || AutoUpdateChecker.updateInProgress) {
            return
        }

        updateCheckRequested = true
        updateCheckInProgress = true
        pendingUpdateVersion = ""
        updateStatus = qsTr("Checking for updates...")
        if (!AutoUpdateChecker.checkForUpdates()) {
            if (!AutoUpdateChecker.supportsUpdateCheck()) {
                updateCheckRequested = false
                updateCheckInProgress = false
                updateStatus = qsTr("Unable to check for updates.")
            }
        }
    }

    function handleUpdateCheckStarted() {
        if (aboutPage.updateCheckRequested) {
            aboutPage.updateCheckInProgress = true
            aboutPage.updateStatus = qsTr("Checking for updates...")
        }
    }

    function handleUpdateAvailable(version, url) {
        aboutPage.pendingUpdateVersion = version
        if (!aboutPage.updateCheckRequested) {
            aboutPage.updateStatus = qsTr("Update available: Version %1").arg(version)
        }
    }

    function handleUpdateCheckFinished(updateAvailable) {
        if (!aboutPage.updateCheckRequested) {
            return
        }

        aboutPage.updateCheckRequested = false
        aboutPage.updateCheckInProgress = false
        aboutPage.updateStatus = updateAvailable && aboutPage.pendingUpdateVersion !== ""
                ? qsTr("Update available: Version %1").arg(aboutPage.pendingUpdateVersion)
                : qsTr("You are using the latest version.")
    }

    function handleUpdateCheckFailed() {
        if (!aboutPage.updateCheckRequested) {
            return
        }

        aboutPage.updateCheckRequested = false
        aboutPage.updateCheckInProgress = false
        aboutPage.updateStatus = qsTr("Unable to check for updates.")
    }

    Component.onCompleted: {
        AutoUpdateChecker.onUpdateCheckStarted.connect(aboutPage.handleUpdateCheckStarted)
        AutoUpdateChecker.onUpdateAvailable.connect(aboutPage.handleUpdateAvailable)
        AutoUpdateChecker.onUpdateCheckFinished.connect(aboutPage.handleUpdateCheckFinished)
        AutoUpdateChecker.onUpdateCheckFailed.connect(aboutPage.handleUpdateCheckFailed)
    }

    Component.onDestruction: {
        AutoUpdateChecker.onUpdateCheckStarted.disconnect(aboutPage.handleUpdateCheckStarted)
        AutoUpdateChecker.onUpdateAvailable.disconnect(aboutPage.handleUpdateAvailable)
        AutoUpdateChecker.onUpdateCheckFinished.disconnect(aboutPage.handleUpdateCheckFinished)
        AutoUpdateChecker.onUpdateCheckFailed.disconnect(aboutPage.handleUpdateCheckFailed)
    }

    PlatformDialogs.FileDialog {
        id: localHotUpdateDialog
        title: aboutPage.zh("选择 SunAndMon 热更新 ZIP 包", qsTr("Select SunAndMon Hot-Update ZIP"))
        fileMode: PlatformDialogs.FileDialog.OpenFile
        nameFilters: [aboutPage.zh("SunAndMon 更新压缩包 (*.zip)", qsTr("SunAndMon update archive (*.zip)"))]
        onAccepted: {
            AutoUpdateChecker.installLocalUpdate(file.toString())
        }
    }

    SettingsCard {
        searchCategory: "about"
        title: qsTr("About")

        Column {
            width: parent.width
            spacing: Theme.spaceMd

            TechTag {
                text: SystemProperties.isSunAndMon
                      ? aboutPage.zh("SunAndMon（日月同辉）", "SunAndMon")
                      : qsTr("AlkaidLab Open Source Community")
                prominent: true
            }

            Text {
                width: parent.width
                text: SystemProperties.isSunAndMon
                      ? (aboutPage.paragraphIndent + aboutPage.zh(
                            "SunAndMon 整合了 Moonlight Qt 客户端、Foundation Sunshine 被控端引擎、内置 P2P 隧道（EasyTier / Tailscale）与中心服务器设备发现，提供一体化远程桌面与串流体验。",
                            "SunAndMon integrates the Moonlight Qt controller, Foundation Sunshine host engine, built-in P2P tunnel (EasyTier / Tailscale), and Center device discovery into one unified remote desktop and streaming suite."))
                      : (aboutPage.paragraphIndent + qsTr("Founded in 2024, AlkaidLab has brought together tens of thousands of users and maintained in-depth dialogue with thousands of them. Guided by extensive real-world feedback, we have developed a systematic understanding of networking and streaming as a whole and built a complete solution spanning video, audio, touch, controllers, networking, and multi-device experiences, with continuous improvements ahead. The ecosystem covers desktop PCs, Android handhelds, Windows handhelds, TVs and set-top boxes, projectors, VR devices, and more, aiming to deliver a more elegant and uncompromising streaming experience for gaming, media, creation, and remote collaboration."))
                color: Theme.textSettingsSubtitle
                font.family: Theme.fontSans
                font.pointSize: Theme.fontSettingsSubtitle + 1
                font.weight: Font.Medium
                wrapMode: Text.Wrap
            }

            Text {
                text: qsTr("Version %1").arg(SystemProperties.versionString)
                color: Theme.text
                font.family: Theme.fontSans
                font.pointSize: Theme.fontRowTitle
                font.weight: Font.DemiBold
            }

            Column {
                width: parent.width
                spacing: Theme.spaceSm

                Text {
                    text: SystemProperties.isSunAndMon
                          ? aboutPage.zh("版本更新与热更新", qsTr("Updates & Hot-Update"))
                          : qsTr("Check for updates")
                    color: Theme.text
                    font.family: Theme.fontSans
                    font.pointSize: Theme.fontRowTitle
                    font.weight: Font.DemiBold
                }

                Flow {
                    width: parent.width
                    spacing: Theme.spaceSm

                    HardButton {
                        text: aboutPage.updateCheckInProgress ? qsTr("Checking...") : qsTr("Check now")
                        primary: !AutoUpdateChecker.updateAvailable
                        enabled: AutoUpdateChecker.supportsUpdateCheck() &&
                            !aboutPage.updateCheckInProgress &&
                            !AutoUpdateChecker.updateInProgress
                        icon.source: "qrc:/res/fluent/tb-update.svg"
                        icon.color: primary ? Theme.accentText : Theme.text
                        icon.width: 16
                        icon.height: 16
                        onClicked: aboutPage.requestUpdateCheck()
                    }

                    HardButton {
                        visible: AutoUpdateChecker.updateAvailable
                        text: AutoUpdateChecker.supportsInAppUpdate()
                              ? aboutPage.zh("一键热更新 (%1)", "Hot Update (%1)").arg(AutoUpdateChecker.latestVersion)
                              : aboutPage.zh("下载更新 (%1)", "Download (%1)").arg(AutoUpdateChecker.latestVersion)
                        primary: true
                        enabled: !AutoUpdateChecker.updateInProgress
                        icon.source: "qrc:/res/fluent/tb-update.svg"
                        icon.color: Theme.accentText
                        icon.width: 16
                        icon.height: 16
                        onClicked: AutoUpdateChecker.installAvailableUpdate()
                    }

                    HardButton {
                        visible: SystemProperties.isSunAndMon && AutoUpdateChecker.supportsInAppUpdate()
                        text: aboutPage.zh("从本地 ZIP 热更新...", "Hot Update from Local ZIP...")
                        enabled: !AutoUpdateChecker.updateInProgress
                        onClicked: localHotUpdateDialog.open()
                    }
                }

                Text {
                    width: parent.width
                    text: AutoUpdateChecker.updateStatusMessage !== ""
                          ? AutoUpdateChecker.updateStatusMessage
                          : aboutPage.updateStatus
                    visible: text !== ""
                    color: Theme.textSettingsSubtitle
                    font.family: Theme.fontSans
                    font.pointSize: Theme.fontSettingsSubtitle
                    wrapMode: Text.Wrap
                }
            }
        }

        ToggleRow {
            visible: SystemProperties.isSunAndMon
            title: aboutPage.zh("包含预发布版本", "Include pre-release versions")
            searchKeywords: "pre-release beta update channel 预发布 热更新 测试版"
            description: aboutPage.zh(
                "除正式稳定版外，同时检查 GitHub 预发布版本（例如预览热更新构建）。",
                "Check GitHub pre-releases (such as preview hot-update builds) in addition to stable releases.")
            checked: StreamingPreferences.includePreReleaseUpdates
            onToggled: function(value) { StreamingPreferences.includePreReleaseUpdates = value }
        }

        Flow {
            width: parent.width
            spacing: Theme.spaceSm

            HardLink {
                text: SystemProperties.isSunAndMon ? aboutPage.zh("GitHub 仓库", "GitHub Repository") : qsTr("Website")
                prominent: true
                onClicked: aboutPage.openExternal(aboutPage.officialSiteUrl)
            }

            HardLink {
                visible: SystemProperties.isSunAndMon
                text: aboutPage.zh("版本发布页", "Releases")
                prominent: true
                onClicked: aboutPage.openExternal(aboutPage.sunAndMonReleasesUrl)
            }

            HardLink {
                visible: !SystemProperties.isSunAndMon
                text: qsTr("Bilibili")
                prominent: true
                onClicked: aboutPage.openExternal(aboutPage.bilibiliUrl)
            }

            HardLink {
                visible: !SystemProperties.isSunAndMon
                text: qsTr("QQ Group")
                prominent: true
                onClicked: aboutPage.openExternal(aboutPage.qqUrl)
            }

            HardLink {
                text: qsTr("Submit Issue")
                prominent: true
                onClicked: aboutPage.openExternal(aboutPage.issueUrl)
            }

            HardLink {
                text: qsTr("Legal notice")
                prominent: true
                onClicked: aboutPage.revealLegalNotice()
            }
        }
    }

    SettingsCard {
        searchCategory: "about"
        title: qsTr("Legal notice")
        subtitle: qsTr("This software is released under GNU GPLv3. The full license and third-party component information are provided below.")

        SettingsRow {
            id: legalInfoRow

            title: "GNU GPL v3.0"
            searchKeywords: "GNU GPL v3.0"

            Flow {
                width: Math.min(300, Math.max(0, legalInfoRow.width - Theme.spaceMd * 2))
                spacing: Theme.spaceSm

                HardLink {
                    text: qsTr("License agreement")
                    onClicked: aboutPage.openExternal(aboutPage.licenseUrl)
                }

                HardLink {
                    text: qsTr("Third-party notice")
                    onClicked: aboutPage.openExternal(aboutPage.noticeUrl)
                }
            }
        }

        SettingsRow {
            id: usbipdcppRow

            title: qsTr("usbipdcpp (LGPL-3.0)")
            searchKeywords: "usbipdcpp (LGPL-3.0)"
            description: qsTr("This product uses usbipdcpp (https://github.com/yunsmall/usbipdcpp), licensed under LGPLv3.")

            Flow {
                width: Math.min(300, Math.max(0, usbipdcppRow.width - Theme.spaceMd * 2))
                spacing: Theme.spaceSm

                HardLink {
                    text: qsTr("Project")
                    onClicked: aboutPage.openExternal(aboutPage.usbipdcppUrl)
                }
            }
        }

        CommunityLicenseNotice {
            width: parent.width
        }
    }

}
