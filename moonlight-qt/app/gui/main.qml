import QtQuick
import QtQuick.Controls
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
import QtQuick.Controls.Material as MaterialStyle

import ComputerManager 1.0
import StreamTabManager 1.0
import AutoUpdateChecker 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0
import WindowPlacement 1.0
import WindowsWindowChrome 1.0

import "theme"
import "Brand.js" as Brand

ApplicationWindow {
    property bool pollingActive: false
    property bool revealAfterFirstFrame: false
    property bool configurationChecksStarted: false
    property bool initialBackgroundChoiceHandled: false
    readonly property bool embeddedChildMode: (typeof isEmbeddedChild === "boolean") && isEmbeddedChild
    readonly property bool compactTabHeader: !embeddedChildMode && StreamTabManager.hasTabs && !StreamTabManager.fullscreenStream
    property int preFullscreenVisibility: Window.Windowed
    readonly property var settingsItem: stackView.currentItem instanceof SettingsView
                                         ? stackView.currentItem : null

    // 搜索快捷键属于主窗口级别，避免焦点落在设置控件或文本框时失效。
    // Windows/Linux 使用 Ctrl+K，macOS 使用 Cmd+K，并且只在设置页注册。
    Shortcut {
        id: settingsSearchShortcut
        sequence: Qt.platform.os === "osx" ? "Meta+K" : "Ctrl+K"
        enabled: window.settingsItem !== null && !liquidTransition.running
        onActivated: window.settingsItem.openSearch()
    }

    Timer {
        id: revealFallbackTimer
        interval: 1000
        repeat: false
        onTriggered: {
            window.revealAfterFirstFrame = false
            window.opacity = 1
        }
    }

    // Set by SettingsView to force the back operation to pop all
    // pages except the initial view. This is required when doing
    // a retranslate() because AppView breaks for some reason.
    property bool clearOnBack: false

    id: window
    // macOS 原生标题栏会居中显示窗口标题，和工具栏面包屑里的字标叠成双标题；
    // 清空原生标题，让工具栏做唯一的品牌位。Windows/Linux 无此问题。
    title: SystemProperties.isDarwin ? "" : Qt.application.displayName
    width: 1280
    height: 640

    WindowPlacement {
        id: windowPlacement
        window: window
        enabled: StreamingPreferences.rememberWindowPosition &&
                 SystemProperties.hasDesktopEnvironment &&
                 (!SystemProperties.isRunningWayland || SystemProperties.isRunningXWayland)
    }

    WindowsWindowChrome {
        id: windowsWindowChrome
        window: window
        titleBar: titleDragRegion
        minimizeToTray: Theme.modern && (Qt.platform.os === "windows" || Qt.platform.os === "linux") && StreamingPreferences.minimizeToTray
        onExitRequested: quitConfirmationDialog.open()
    }

    // Windows 保留标准顶层窗口状态和系统命令，只由 WindowsWindowChrome 移除
    // 非客户区并绘制自定义标题栏。macOS 保留扩展客户区，Linux 使用原生窗口边框。
    flags: Qt.platform.os === "osx"
           ? Qt.Window | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
           : Qt.Window

    // 加了上面那两个 flag 之后窗口的可绘制区域顶到了最上沿，但 ApplicationWindow 仍然
    // 把 contentItem 往下缩了一个安全区（实测 macOS 上 contentItem.y = 32，正好是系统
    // 标题栏那条带子的高度）。结果是工具栏其实从 32pt 才开始，上面留着一条空带。
    //
    // 我们要的是「工具栏本身就是标题栏」，所以把顶层的几层都往上顶回去，一切仍然从
    // 窗口真正的顶边开始量。全屏时 contentItem.y 会变回 0，这个绑定跟着走。
    readonly property real chromeInset: contentItem.y

    onFrameSwapped: {
        if (revealAfterFirstFrame) {
            revealAfterFirstFrame = false
            revealFallbackTimer.stop()
            opacity = 1
        }
    }

    // FluentWinUI3's ApplicationWindow is just "color: palette.window", and on macOS
    // that palette follows the system appearance regardless of the color scheme we
    // ask for. Pin it so pages we haven't given a background of their own (the
    // connection spinner, the quit page) are never white-on-white.
    color: Theme.ink

    // This function runs prior to creation of the initial StackView item
    function doEarlyInit() {
        // Override the background color to Material 2 colors for Qt 6.5+
        // in order to improve contrast between GFE's placeholder box art
        // and the background of the app grid.
        if (!Theme.modern && SystemProperties.usesMaterial3Theme) {
            MaterialStyle.Material.background = "#303030"
        }

        SdlGamepadKeyNavigation.enable()
    }

    function startConfigurationChecks() {
        if (configurationChecksStarted) {
            return
        }
        configurationChecksStarted = true

        if (!runConfigChecks) {
            return
        }

        if (SystemProperties.isWow64) {
            wow64Dialog.open()
        }

        // Hardware acceleration and unmapped gamepads are checked asynchronously.
        SystemProperties.hasHardwareAccelerationChanged.connect(hasHardwareAccelerationChanged)
        SystemProperties.unmappedGamepadsChanged.connect(hasUnmappedGamepadsChanged)
        SystemProperties.startAsyncLoad()
    }

    function commitInitialBackgroundSource(source) {
        if (initialBackgroundChoiceHandled) {
            return
        }

        initialBackgroundChoiceHandled = true
        StreamingPreferences.backgroundSource = source
        StreamingPreferences.save()
    }

    Component.onCompleted: {
        if (embeddedChildMode) {
            // Headless child mode for single-window embedded stream tabs:
            // never flash or show a second top-level Qt window on the taskbar.
            return
        }
        StreamTabManager.bindHostWindow(window)
        // Always fit the initial window to the current screen. When the preference
        // is enabled, restore the last normal window geometry before showing it.
        windowsWindowChrome.activate()
        var startMaximized = windowPlacement.restore(
                    StreamingPreferences.uiDisplayMode === StreamingPreferences.UI_MAXIMIZED)

        // Show the window according to the user's preferences
        if (SystemProperties.hasDesktopEnvironment) {
            if (StreamingPreferences.uiDisplayMode === StreamingPreferences.UI_FULLSCREEN) {
                window.showFullScreen()
            }
            else if (startMaximized) {
                if (Qt.platform.os === "windows") {
                    window.opacity = 0
                    window.revealAfterFirstFrame = true
                    revealFallbackTimer.start()
                }
                window.showMaximized()
            }
            else {
                window.show()
            }
        } else {
            window.showFullScreen()
        }

        // Let a fresh install choose its background before any other startup
        // warning is opened. Existing installs and CLI launches skip this step.
        if (runConfigChecks && !StreamingPreferences.backgroundSetupCompleted) {
            if (Theme.modern) {
                commitInitialBackgroundSource(StreamingPreferences.BGS_NONE)
                startConfigurationChecks()
            } else {
                Qt.callLater(function() { backgroundSourceDialog.open() })
            }
        }
        else {
            startConfigurationChecks()
        }
    }

    readonly property bool confirmWindowExit: !embeddedChildMode && !ComputerManager.center.secondaryInstance &&
                                              (Qt.platform.os === "windows" || Qt.platform.os === "linux") &&
                                              ComputerManager.center.hostAvailable
    property bool exitConfirmed: false

    Connections {
        target: StreamTabManager
        function onFullscreenStreamChanged() {
            if (window.embeddedChildMode) {
                return
            }
            if (StreamTabManager.fullscreenStream) {
                window.preFullscreenVisibility = (window.visibility === Window.Maximized)
                    ? Window.Maximized : Window.Windowed
                toolBar.shown = false
                window.showFullScreen()
            } else {
                toolBar.shown = true
                if (window.visibility === Window.FullScreen) {
                    if (window.preFullscreenVisibility === Window.Maximized) {
                        window.showMaximized()
                    } else {
                        window.showNormal()
                    }
                }
            }
            streamViewportContainer.syncViewport()
            viewportSyncTimer.restart()
        }
    }

    Timer {
        id: viewportSyncTimer
        interval: 60
        repeat: false
        onTriggered: streamViewportContainer.syncViewport()
    }

    onClosing: function(event) {
        if (confirmWindowExit && !exitConfirmed) {
            event.accepted = false
            Qt.callLater(function() {
                if (quitConfirmationDialog.visible)
                    return
                if (!StreamingPreferences.closeToTray || !windowsWindowChrome.hideToTray())
                    quitConfirmationDialog.open()
            })
            return
        }
        StreamTabManager.closeAllTabs()
        windowPlacement.flush()
    }

    function hasHardwareAccelerationChanged() {
        if (!SystemProperties.hasHardwareAcceleration && StreamingPreferences.videoDecoderSelection !== StreamingPreferences.VDS_FORCE_SOFTWARE) {
            if (SystemProperties.isRunningXWayland) {
                xWaylandDialog.open()
            }
            else {
                noHwDecoderDialog.open()
            }
        }
    }

    function hasUnmappedGamepadsChanged() {
        if (SystemProperties.unmappedGamepads) {
            unmappedGamepadDialog.unmappedGamepads = SystemProperties.unmappedGamepads
            unmappedGamepadDialog.open()
        }
    }

    // It would be better to use TextMetrics here, but it always lays out
    // the text slightly more compactly than real Text does in ToolTip,
    // causing unexpected line breaks to be inserted
    Text {
        id: tooltipTextLayoutHelper
        visible: false
        font: ToolTip.toolTip.font
        text: ToolTip.toolTip.text
    }

    function changeAppearance(dark) {
        liquidTransition.allowed = Theme.modern && windowsWindowChrome.clientAreaAnimationsEnabled()
        liquidTransition.play(() => {
            StreamingPreferences.darkAppearance = dark
            StreamingPreferences.save()
        }, !dark, dark)
    }

    function goBack() {
        if (stackView.depth <= 1 || liquidTransition.running) {
            return
        }
        const popPage = function(operation) {
            const target = clearOnBack ? null : stackView.get(stackView.depth - 2)
            clearOnBack = false
            stackView.pop(target, operation)
        }
        popPage(StackView.PopTransition)
    }

    // 全局壁纸。PcView 负责抓取、缓存和刷新，抓到之后写回这里，
    // 这样连接进度页、退出页、设置页共用同一张背景，而不是各自一片纯色。
    property string backgroundImageUrl: ""

    // PcView / AppView / SettingsView 各自已经按自己的配方铺了一层壁纸（半透明 + 压暗），
    // 它们背后再垫一张全尺寸原图的话两层会错位叠在一起。所以这层只服务于自己不画背景的页面
    // ——连接进度页、退出页。
    readonly property bool showGlobalBackground:
        !(stackView.currentItem && stackView.currentItem.usesOwnBackground === true)

    // 屏幕键盘单例,HardTextField 组件统一从这里取(见 theme/HardTextField.qml)
    readonly property alias gamepadOsk: gamepadKeyboard

    Loader {
        anchors.fill: parent
        anchors.topMargin: -window.chromeInset
        z: -3
        active: Theme.modern
        visible: active && !window.showGlobalBackground
        source: "GlassBackdrop.qml"
        onLoaded: {
            item.wallpaper = Qt.binding(() => window.backgroundImageUrl)
            item.wallpaperStrength = Qt.binding(() => StreamingPreferences.backgroundSource === StreamingPreferences.BGS_NONE
                ? 0 : 0.45 * (1 - StreamingPreferences.backgroundOverlayOpacity / 100))
        }
    }

    Image {
        anchors.fill: parent
        anchors.topMargin: -window.chromeInset
        source: window.backgroundImageUrl
        visible: source != "" && window.showGlobalBackground
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
        z: -3
    }

    // 压暗壁纸，保证上层内容的可读性；强度和各个自绘背景页使用同一设置。
    Rectangle {
        anchors.fill: parent
        anchors.topMargin: -window.chromeInset
        color: Qt.rgba(Theme.ink.r, Theme.ink.g, Theme.ink.b,
                       StreamingPreferences.backgroundOverlayOpacity / 100.0)
        visible: window.showGlobalBackground
        z: -2
    }

    StackView {
        id: stackView
        anchors.fill: parent
        // 各页自己按 72 让出工具栏的高度，那个 72 是从窗口顶边算的
        anchors.topMargin: -window.chromeInset
        visible: !StreamTabManager.streamTabActive
        focus: !StreamTabManager.streamTabActive

        // 切页动效：neo-brutalism 要更短更机械，所以不缩放（缩放读起来是「软」的），
        // 改成 12px 横向位移 + 淡入，150ms OutQuad。
        pushEnter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durNormal; easing.type: Theme.easing }
                NumberAnimation { property: "x"; from: 12; to: 0; duration: Theme.durNormal; easing.type: Theme.easing }
            }
        }
        pushExit: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.durFast; easing.type: Easing.InQuad }
                NumberAnimation { property: "x"; from: 0; to: -12; duration: Theme.durFast; easing.type: Easing.InQuad }
            }
        }
        popEnter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durNormal; easing.type: Theme.easing }
                NumberAnimation { property: "x"; from: -12; to: 0; duration: Theme.durNormal; easing.type: Theme.easing }
            }
        }
        popExit: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.durFast; easing.type: Easing.InQuad }
                NumberAnimation { property: "x"; from: 0; to: 12; duration: Theme.durFast; easing.type: Easing.InQuad }
            }
        }

        // This configures the maximum width of the singleton attached QML ToolTip. If left unconstrained,
        // it will never insert a line break and just extend on forever.
        ToolTip.toolTip.contentWidth: Math.min(tooltipTextLayoutHelper.width, 400)
        ToolTip.toolTip.margins: 8

        ToolTip.toolTip.onVisibleChanged: {
            if (ToolTip.toolTip.visible) ToolTip.toolTip.y = toolBar.height - 5
        }

        Component.onCompleted: {
            // Perform our early initialization before constructing
            // the initial view and pushing it to the StackView
            doEarlyInit()
            push(initialView)
        }

        onCurrentItemChanged: {
            // Ensure focus travels to the next view when going back
            if (currentItem) {
                currentItem.forceActiveFocus()
            }
        }

        Keys.onEscapePressed: {
            if (depth > 1) {
                goBack()
            }
            else {
                quitConfirmationDialog.open()
            }
        }

        Keys.onBackPressed: {
            if (depth > 1) {
                goBack()
            }
            else {
                quitConfirmationDialog.open()
            }
        }

        Keys.onMenuPressed: {
            settingsButton.clicked()
        }

        // This is a keypress we've reserved for letting the
        // SdlGamepadKeyNavigation object tell us to show settings
        // when Menu is consumed by a focused control.
        Keys.onHangupPressed: {
            settingsButton.clicked()
        }
    }

    // This timer keeps us polling for 5 minutes of inactivity
    // to allow the user to work with Moonlight on a second display
    // while dealing with configuration issues. This will ensure
    // machines come online even if the input focus isn't on Moonlight.
    Timer {
        id: inactivityTimer
        interval: 5 * 60000
        onTriggered: {
            if (!active && pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
    }

    onVisibleChanged: {
        // When we become invisible while streaming is going on,
        // stop polling immediately.
        if (!visible) {
            inactivityTimer.stop()

            if (pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
        else if (active) {
            // When we become visible and active again, start polling
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }

        // Poll for gamepad input only when the window is in focus and not streaming in a tab
        SdlGamepadKeyNavigation.notifyWindowFocus(visible && active && !StreamTabManager.streamTabActive)

        if (!window.embeddedChildMode && StreamTabManager.hasTabs) {
            StreamTabManager.notifyHostWindowVisibilityChanged(
                visible && visibility !== Window.Minimized && visibility !== Window.Hidden)
        }
    }

    onVisibilityChanged: {
        if (!window.embeddedChildMode && StreamTabManager.hasTabs) {
            StreamTabManager.notifyHostWindowVisibilityChanged(
                visible && visibility !== Window.Minimized && visibility !== Window.Hidden)
        }
    }

    onActiveChanged: {
        if (active) {
            // Stop the inactivity timer
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }
        else {
            // Start the inactivity timer to stop polling
            // if focus does not return within a few minutes.
            inactivityTimer.restart()
        }

        // Poll for gamepad input only when the window is in focus and not streaming in a tab
        SdlGamepadKeyNavigation.notifyWindowFocus(visible && active && !StreamTabManager.streamTabActive)

        if (!window.embeddedChildMode && StreamTabManager.streamTabActive) {
            StreamTabManager.notifyHostWindowFocusChanged(active)
        }
    }

    onXChanged: {
        if (!window.embeddedChildMode && StreamTabManager.streamTabActive) {
            StreamTabManager.notifyHostWindowMoved()
        }
    }

    onYChanged: {
        if (!window.embeddedChildMode && StreamTabManager.streamTabActive) {
            StreamTabManager.notifyHostWindowMoved()
        }
    }

    function openAddPc() { addPcDialog.open() }

    function openSettings() {
        if (settingsItem || liquidTransition.running) {
            return
        }
        const showSettings = function(operation) {
            const existing = stackView.find(item => item instanceof SettingsView)
            if (existing) {
                stackView.pop(existing, operation)
            } else {
                stackView.push("qrc:/gui/SettingsView.qml", {}, operation)
            }
        }
        showSettings(StackView.PushTransition)
    }

    LiquidPageTransition {
        id: liquidTransition
        anchors.fill: parent
        anchors.topMargin: toolBar.height - window.chromeInset
        z: 2
    }

    // 添加工具栏作为浮动元素
    ToolBar {
        id: toolBar

        // 各个 segue 页面用 shown 而不是直接写 visible：直接 visible=false 的话
        // 工具栏会「啪」地消失，而 visible 变假之后就不再渲染，opacity 动画也没机会跑。
        property bool shown: true
        readonly property bool lightChrome: Theme.modern
        opacity: shown ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            NumberAnimation { duration: 220; easing.type: Easing.InOutQuad }
        }

        height: StreamTabManager.fullscreenStream ? 0 : (window.compactTabHeader ? 40 : 56)
        anchors.top: parent.top
        anchors.topMargin: -window.chromeInset
        anchors.left: parent.left
        anchors.right: parent.right
        z: 3

        // Qt 6.9 起 Control 会把安全区当成 padding 自动加上去（实测这里 topPadding
        // 被设成了 32），于是这条 bar 里能用的高度只剩 24，字标和按钮被挤到下半部分，
        // 和红绿灯对不齐。这层 padding 的用意是「别把内容放到刘海/标题栏底下」，而我们
        // 恰恰是故意把工具栏当标题栏用的 —— 红绿灯的位置由下面 windowButtonInsetLeft
        // 自己让，所以这里四边都归零。
        topPadding: 0
        bottomPadding: 0
        leftPadding: 0
        rightPadding: 0

        // 给系统窗口按钮让位。Qt 的 SafeArea 只报了标题栏那条带子的高度
        // （实测 macOS 上 top=32、left=0），没有给出按钮的水平占位，只能自己留。
        //
        // macOS：红绿灯被 macwindowchrome.mm 挪到了 x=20 起（整组 60pt 宽，到 80 结束），
        // 这里再留一格间距 —— 20 + 60 + 20 = 100，减掉 RowLayout 自带的 spaceLg(16)
        // 就是 84。改动那边的 kButtonLeftMargin 时这个数要跟着改。
        //
        // Windows：右边是三颗 44px 的自绘窗口按钮。
        //
        // 全屏时两边都不留：macOS 全屏会把红绿灯藏起来（要鼠标移到顶边才浮出来），
        // 这时候还留着那段让位就是一段莫名其妙的空档 —— 界面全屏是个偏好项，
        // 真有人这么用。
        readonly property bool windowChromeVisible: window.visibility !== Window.FullScreen
        readonly property int customWindowControlsWidth: 132
        readonly property int windowButtonInsetLeft:
            (SystemProperties.isDarwin && windowChromeVisible) ? 84 : 0
        readonly property int windowButtonInsetRight:
            (Qt.platform.os === "windows" && windowChromeVisible)
                ? customWindowControlsWidth : 0

        // 以前这是一条「浮」在壁纸上的透明工具栏（topMargin: 5 + transparent 背景）。
        // 新风格里它是一条真正的 bar：贴住窗口顶边、底部一条 1px 分隔线，页面内容从
        // 线下面开始。底色留一半透明度让壁纸透上来 —— 全不透明的话这条 bar 会像一块
        // 贴在窗口上的黑板，和下面的壁纸完全割裂。不加模糊（这套风格里没有毛玻璃）。
        background: Rectangle {
            color: toolBar.lightChrome ? Theme.railSurface : Qt.rgba(Theme.ink.r, Theme.ink.g, Theme.ink.b, 0.55)

            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: 1
                color: Theme.line
            }
        }

        RowLayout {
            spacing: window.compactTabHeader ? 6 : Theme.spaceSm
            anchors.leftMargin: (window.compactTabHeader ? 8 : Theme.spaceLg) + toolBar.windowButtonInsetLeft
            anchors.rightMargin: (window.compactTabHeader ? 8 : Theme.spaceLg) + toolBar.windowButtonInsetRight
            anchors.fill: parent

            NavigableToolButton {
                // Only make the button visible if the user has navigated somewhere.
                visible: stackView.depth > 1 && !StreamTabManager.streamTabActive

                iconSource: "qrc:/res/fluent/tb-back.svg"

                onClicked: goBack()

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocusReason)
                }
            }

            // 单行多标签模式：固定首页标签「设备列表」+ 可关闭的串流标签（含连接链路标识）
            Rectangle {
                id: homeTabBtn
                visible: window.compactTabHeader
                Layout.preferredHeight: 28
                Layout.preferredWidth: homeTabRow.implicitWidth + 18
                radius: Theme.radiusControl
                color: !StreamTabManager.streamTabActive ? Theme.accentSoft
                     : homeTabMouse.containsMouse ? Theme.surface2 : "transparent"
                border.color: !StreamTabManager.streamTabActive ? Theme.accent : "transparent"
                border.width: 1

                Row {
                    id: homeTabRow
                    anchors.centerIn: parent
                    spacing: 5
                    Text {
                        text: qsTr("Devices")
                        color: !StreamTabManager.streamTabActive ? Theme.text : Theme.textDim
                        font.family: Theme.fontSans
                        font.pixelSize: 12
                        font.weight: !StreamTabManager.streamTabActive ? Font.DemiBold : Font.Normal
                    }
                }

                MouseArea {
                    id: homeTabMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: StreamTabManager.activateTab("")
                }
            }

            Rectangle {
                visible: window.compactTabHeader
                Layout.preferredWidth: 1
                Layout.preferredHeight: 16
                color: Theme.line
            }

            Flickable {
                id: tabStripFlick
                visible: window.compactTabHeader
                Layout.preferredWidth: Math.min(tabStripRow.implicitWidth, Math.max(160, toolBar.width - 380))
                Layout.maximumWidth: Math.max(160, toolBar.width - 380)
                Layout.fillHeight: true
                contentWidth: tabStripRow.implicitWidth
                contentHeight: height
                clip: true
                flickableDirection: Flickable.HorizontalFlick

                Row {
                    id: tabStripRow
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 5

                    Repeater {
                        model: StreamTabManager.tabs

                        Rectangle {
                            id: streamTabPill
                            required property var modelData
                            readonly property bool isActive: StreamTabManager.activeTabId === modelData.id
                            readonly property string routeType: modelData.routeType || ""
                            readonly property string routeLabel: modelData.routeLabel || ""
                            readonly property color routeColor: {
                                if (routeType === "lan") return "#2ea043"
                                if (routeType === "easytier") return "#38bdf8"
                                if (routeType === "tailscale") return "#a78bfa"
                                if (routeType === "direct") return "#f59e0b"
                                return Theme.accent
                            }
                            height: 28
                            width: Math.min(300, Math.max(124, tabContentRow.implicitWidth + 16))
                            radius: Theme.radiusControl
                            color: isActive ? Theme.surface
                                 : tabArea.containsMouse ? Theme.surface2 : "transparent"
                            border.color: isActive ? Theme.accent : Theme.line
                            border.width: 1

                            MouseArea {
                                id: tabArea
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                                cursorShape: Qt.PointingHandCursor
                                onClicked: function(mouse) {
                                    if (mouse.button === Qt.MiddleButton) {
                                        StreamTabManager.closeTab(streamTabPill.modelData.id)
                                    } else {
                                        StreamTabManager.activateTab(streamTabPill.modelData.id)
                                    }
                                }
                            }

                            RowLayout {
                                id: tabContentRow
                                anchors.fill: parent
                                anchors.leftMargin: 9
                                anchors.rightMargin: 5
                                spacing: 6

                                Rectangle {
                                    Layout.preferredWidth: 7
                                    Layout.preferredHeight: 7
                                    radius: 4
                                    color: streamTabPill.modelData.state === "streaming" ? Theme.success
                                         : streamTabPill.modelData.state === "error" ? Theme.danger
                                         : Theme.accent
                                }

                                Text {
                                    Layout.fillWidth: true
                                    Layout.maximumWidth: 130
                                    text: streamTabPill.modelData.title
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    color: streamTabPill.isActive ? Theme.text : Theme.textDim
                                    font.family: Theme.fontSans
                                    font.pixelSize: 12
                                    font.weight: streamTabPill.isActive ? Font.DemiBold : Font.Normal
                                }

                                Rectangle {
                                    visible: streamTabPill.routeLabel.length > 0
                                    Layout.preferredHeight: 18
                                    Layout.preferredWidth: routeBadgeText.implicitWidth + 10
                                    radius: 9
                                    color: Qt.rgba(streamTabPill.routeColor.r, streamTabPill.routeColor.g, streamTabPill.routeColor.b, 0.16)
                                    border.color: Qt.rgba(streamTabPill.routeColor.r, streamTabPill.routeColor.g, streamTabPill.routeColor.b, 0.45)
                                    border.width: 1

                                    Text {
                                        id: routeBadgeText
                                        anchors.centerIn: parent
                                        text: streamTabPill.routeLabel
                                        textFormat: Text.PlainText
                                        color: streamTabPill.routeColor
                                        font.family: Theme.fontSans
                                        font.pixelSize: 10
                                        font.weight: Font.DemiBold
                                    }
                                }

                                Rectangle {
                                    Layout.preferredWidth: 18
                                    Layout.preferredHeight: 18
                                    radius: 4
                                    color: tabCloseMouse.containsMouse ? Theme.danger : "transparent"

                                    Text {
                                        anchors.centerIn: parent
                                        text: "×"
                                        color: tabCloseMouse.containsMouse ? "#ffffff" : Theme.textDim
                                        font.pixelSize: 13
                                        font.weight: Font.Bold
                                    }

                                    MouseArea {
                                        id: tabCloseMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: StreamTabManager.closeTab(streamTabPill.modelData.id)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // 标题区域占满工具栏中所有非按钮空间。Windows 读取这块区域做原生
            // 非客户区命中；Linux 继续使用下面的 MouseArea；macOS 由 AppKit 接管。
            Item {
                id: titleDragRegion
                Layout.fillHeight: true
                Layout.fillWidth: true
                Layout.minimumWidth: 28

                RowLayout {
                    visible: !window.compactTabHeader
                    anchors.fill: parent
                    spacing: Theme.spaceSm

                    Text {
                        id: wordmark
                        visible: toolBar.lightChrome || toolBar.width > 700
                        text: toolBar.lightChrome ? "SunAndMon" : "MOONLIGHT V+ FOR PC"
                        color: Theme.text
                        font.family: Theme.fontSans
                        font.pointSize: Theme.fontCardTitle
                        font.weight: Font.ExtraBold
                        font.letterSpacing: toolBar.lightChrome ? 0 : Theme.tracking(Theme.fontCardTitle, 0.1)
                        verticalAlignment: Text.AlignVCenter
                        Layout.fillHeight: true
                    }

                    Text {
                        visible: wordmark.visible && titleRowLabel.visible
                        text: "/"
                        color: Theme.textFaint
                        font.family: Theme.fontMono
                        font.pointSize: Theme.fontCardTitle
                        verticalAlignment: Text.AlignVCenter
                        Layout.fillHeight: true
                        Layout.leftMargin: Theme.spaceXs
                        Layout.rightMargin: Theme.spaceXs
                    }

                    Text {
                        id: titleRowLabel
                        visible: !toolBar.lightChrome || stackView.depth > 1
                        text: stackView.currentItem ? stackView.currentItem.objectName : ""
                        color: toolBar.lightChrome ? Theme.textDim : Theme.accent
                        font.family: Theme.fontSans
                        font.pointSize: Theme.fontRowTitle
                        font.weight: toolBar.lightChrome ? Font.Medium : Font.Bold
                        font.capitalization: toolBar.lightChrome ? Font.MixedCase : Font.AllUppercase
                        font.letterSpacing: toolBar.lightChrome ? 0 : Theme.tracking(Theme.fontRowTitle, 0.14)
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                        Layout.fillHeight: true
                        Layout.fillWidth: true
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    enabled: Qt.platform.os !== "windows" && !SystemProperties.isDarwin

                    property point pressPosition
                    property bool systemMoveStarted: false

                    onPressed: function(mouse) {
                        pressPosition = Qt.point(mouse.x, mouse.y)
                        systemMoveStarted = false
                    }
                    onPositionChanged: function(mouse) {
                        if (!pressed || systemMoveStarted) {
                            return
                        }

                        var deltaX = Math.abs(mouse.x - pressPosition.x)
                        var deltaY = Math.abs(mouse.y - pressPosition.y)
                        if (Math.max(deltaX, deltaY) >= Qt.styleHints.startDragDistance) {
                            systemMoveStarted = true
                            window.startSystemMove()
                        }
                    }
                    onDoubleClicked: {
                        if (window.visibility === Window.Maximized) {
                            window.showNormal()
                        }
                        else {
                            window.showMaximized()
                        }
                    }
                }

            }

            Text {
                id: versionLabel
                visible: !Theme.modern && stackView.currentItem instanceof SettingsView
                text: qsTr("Version %1").arg(SystemProperties.versionString)
                color: Theme.textDim
                font.family: Theme.fontMono
                font.pointSize: Theme.fontCaption
                horizontalAlignment: Qt.AlignRight
                verticalAlignment: Text.AlignVCenter
                Layout.fillHeight: true
                Layout.rightMargin: Theme.spaceSm
            }

            NavigableToolButton {
                id: qqButton
                visible: !Theme.modern && SystemProperties.hasBrowser &&
                         stackView.currentItem instanceof SettingsView

                iconSource: "qrc:/res/qq-2.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                // 源串必须是英文：这个仓库的源语言是 en_GB，中文源串会变成 28 个
                // 语言包里的 msgid，而且全都 unfinished —— 英语用户看到的就是那四个
                // 中文字。梗放到 zh_CN 的译文里，两边都能要。
                ToolTip.text: qsTr("Join our QQ group")

                // TODO need to make sure browser is brought to foreground.
                onClicked: Qt.openUrlExternally("https://qm.qq.com/cgi-bin/qm/qr?k=wI7aTvDQdd900n1L_wjjJw3qNP0yOgUa&jump_from=webapi&authKey=CDBn7sGy7HpCKYTcFmoEdNuG/zmkrBWUC/W5A/oZZycKzXwuO/XFCA97IpJRktj3");

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocusReason)
                }
            }

            NavigableToolButton {
                id: addPcButton
                visible: !StreamTabManager.streamTabActive && (stackView.currentItem instanceof PcView)

                iconSource:  "qrc:/res/fluent/tb-add-pc.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Add PC manually") + (newPcShortcut.nativeText ? (" ("+newPcShortcut.nativeText+")") : "")

                Shortcut {
                    id: newPcShortcut
                    sequence: StandardKey.New
                    enabled: addPcButton.visible && !StreamTabManager.streamTabActive
                    onActivated: addPcButton.clicked()
                }

                onClicked: {
                    addPcDialog.open()
                }

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocusReason)
                }
            }

            NavigableToolButton {
                property string browserUrl: ""

                id: updateButton

                iconSource: "qrc:/res/fluent/tb-update.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered || visible

                // Invisible until we get a callback notifying us that
                // an update is available
                visible: false

                onClicked: {
                    if (AutoUpdateChecker.supportsInAppUpdate()) {
                        portableUpdateDialog.text = qsTr("Preparing update...")
                        portableUpdateDialog.open()
                        AutoUpdateChecker.installUpdate(browserUrl)
                    }
                    else if (SystemProperties.hasBrowser) {
                        Qt.openUrlExternally(browserUrl);
                    }
                }

                function updateAvailable(version, url)
                {
                    ToolTip.text = Brand.text(qsTr("Update available for Moonlight: Version %1")).arg(version)
                    updateButton.browserUrl = url
                    updateButton.visible = true
                }

                function portableUpdateStatusChanged(message)
                {
                    portableUpdateDialog.text = message
                    if (!portableUpdateDialog.visible) {
                        portableUpdateDialog.open()
                    }
                }

                function portableUpdateFailed(message)
                {
                    portableUpdateDialog.close()
                    portableUpdateErrorDialog.text = message
                    portableUpdateErrorDialog.open()
                }

                Component.onCompleted: {
                    AutoUpdateChecker.onUpdateAvailable.connect(updateAvailable)
                    AutoUpdateChecker.onPortableUpdateStatusChanged.connect(portableUpdateStatusChanged)
                    AutoUpdateChecker.onPortableUpdateFailed.connect(portableUpdateFailed)
                    if (StreamingPreferences.autoUpdateCheck) {
                        AutoUpdateChecker.start()
                    }
                }

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocusReason)
                }
            }

            NavigableToolButton {
                id: helpButton
                visible: SystemProperties.hasBrowser && !window.compactTabHeader

                iconSource: "qrc:/res/fluent/tb-help.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Help") + (helpShortcut.nativeText ? (" ("+helpShortcut.nativeText+")") : "")

                Shortcut {
                    id: helpShortcut
                    sequence: StandardKey.HelpContents
                    enabled: helpButton.visible && !StreamTabManager.streamTabActive
                    onActivated: helpButton.clicked()
                }

                // TODO need to make sure browser is brought to foreground.
                onClicked: Qt.openUrlExternally("https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide");

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocusReason)
                }
            }

            NavigableToolButton {
                id: ipSettingsButton
                visible: stackView.currentItem instanceof AppView &&
                         stackView.currentItem.hasMultipleAddresses

                iconSource: "qrc:/res/fluent/tb-network.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Connection IP")

                onClicked: {
                    if (stackView.currentItem.openIpDialog) {
                        stackView.currentItem.openIpDialog()
                    }
                }

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocusReason)
                }
            }

            NavigableToolButton {
                id: displaySettingsButton
                visible: stackView.currentItem instanceof AppView

                iconSource: "qrc:/res/fluent/tb-display.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Display Settings")

                onClicked: {
                    if (stackView.currentItem.openDisplayDialog) {
                        stackView.currentItem.openDisplayDialog()
                    }
                }

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocusReason)
                }
            }

            NavigableToolButton {
                id: settingsButton

                visible: !StreamTabManager.streamTabActive && !(stackView.currentItem instanceof SettingsView)

                iconSource:  "qrc:/res/fluent/tb-settings.svg"

                onClicked: window.openSettings()

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocusReason)
                }

                Shortcut {
                    id: settingsShortcut
                    sequence: StandardKey.Preferences
                    // 设置页隐藏入口或串流标签激活时停用快捷键，避免抢走串流键盘输入。
                    enabled: settingsButton.visible && !StreamTabManager.streamTabActive
                    onActivated: settingsButton.clicked()
                }

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Settings") + (settingsShortcut.nativeText ? (" ("+settingsShortcut.nativeText+")") : "")
            }

            Rectangle {
                visible: window.compactTabHeader && StreamTabManager.streamTabActive
                Layout.preferredHeight: 26
                Layout.preferredWidth: ccLabel.implicitWidth + 16
                radius: Theme.radiusControl
                color: ccBtnMouse.containsMouse ? Theme.accentSoft : "transparent"
                border.color: Theme.line
                border.width: 1

                Text {
                    id: ccLabel
                    anchors.centerIn: parent
                    text: qsTr("Control Center")
                    color: Theme.text
                    font.family: Theme.fontSans
                    font.pixelSize: 12
                }

                MouseArea {
                    id: ccBtnMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: StreamTabManager.toggleActiveMenu()
                }
            }

            Rectangle {
                visible: window.compactTabHeader && StreamTabManager.streamTabActive
                Layout.preferredHeight: 26
                Layout.preferredWidth: fsLabel.implicitWidth + 16
                radius: Theme.radiusControl
                color: fsBtnMouse.containsMouse ? Theme.accentSoft : "transparent"
                border.color: Theme.line
                border.width: 1

                Text {
                    id: fsLabel
                    anchors.centerIn: parent
                    text: qsTr("Fullscreen")
                    color: Theme.text
                    font.family: Theme.fontSans
                    font.pixelSize: 12
                }

                MouseArea {
                    id: fsBtnMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: StreamTabManager.setFullscreenStream(true)
                }
            }
        }

        Row {
            id: windowControls
            readonly property bool lightChrome: toolBar.lightChrome
            visible: Qt.platform.os === "windows" && toolBar.windowChromeVisible
            anchors.top: parent.top
            anchors.right: parent.right
            height: parent.height
            z: 2

            WindowControlButton {
                controlType: "minimize"
                accessibleName: qsTr("Minimize")
                highlightColor: Theme.acid
                onClicked: windowsWindowChrome.minimize()
            }

            WindowControlButton {
                controlType: windowsWindowChrome.maximized ? "restore" : "maximize"
                accessibleName: windowsWindowChrome.maximized
                                ? qsTr("Restore") : qsTr("Maximize")
                highlightColor: Theme.accent
                onClicked: windowsWindowChrome.toggleMaximized()
            }

            WindowControlButton {
                controlType: "close"
                accessibleName: qsTr("Close")
                highlightColor: Theme.danger
                onClicked: windowsWindowChrome.close()
            }
        }
    }

    // 嵌入式串流画面视口：同步像素矩形给当前激活的子进程 SDL 窗口，并在连接中/出错时展示状态页。
    Item {
        id: streamViewportContainer
        x: 0
        y: StreamTabManager.fullscreenStream ? 0 : (toolBar.height - window.chromeInset)
        width: window.width
        height: Math.max(120, window.height - y)
        visible: !window.embeddedChildMode && StreamTabManager.streamTabActive
        z: 2

        function syncViewport() {
            if (window.embeddedChildMode) {
                return
            }
            StreamTabManager.updateViewport(x, y, width, height)
        }

        onXChanged: syncViewport()
        onYChanged: syncViewport()
        onWidthChanged: syncViewport()
        onHeightChanged: syncViewport()
        onVisibleChanged: syncViewport()

        Connections {
            target: window
            function onVisibilityChanged() { Qt.callLater(streamViewportContainer.syncViewport); viewportSyncTimer.restart() }
            function onWidthChanged() { Qt.callLater(streamViewportContainer.syncViewport) }
            function onHeightChanged() { Qt.callLater(streamViewportContainer.syncViewport) }
        }

        Connections {
            target: toolBar
            function onHeightChanged() { Qt.callLater(streamViewportContainer.syncViewport) }
        }

        Connections {
            target: StreamTabManager
            function onChanged() { Qt.callLater(streamViewportContainer.syncViewport) }
        }

        Rectangle {
            anchors.fill: parent
            color: "#0b1220"
            visible: StreamTabManager.streamTabActive &&
                     (!StreamTabManager.activeTab || StreamTabManager.activeTab.state !== "streaming")

            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width - 64, 480)
                spacing: 14

                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: StreamTabManager.activeTab ? (StreamTabManager.activeTab.title || "") : ""
                    color: "#eef5ff"
                    font.family: Theme.fontSans
                    font.pixelSize: 22
                    font.weight: Font.Bold
                }

                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: {
                        const tab = StreamTabManager.activeTab
                        if (!tab) return ""
                        if (tab.state === "error") return tab.errorText || qsTr("Connection failed.")
                        return tab.statusText || qsTr("Connecting…")
                    }
                    color: StreamTabManager.activeTab && StreamTabManager.activeTab.state === "error"
                           ? "#ff7b72" : "#a8bbd2"
                    font.family: Theme.fontSans
                    font.pixelSize: 13
                }

                HardProgress {
                    Layout.fillWidth: true
                    visible: StreamTabManager.activeTab && StreamTabManager.activeTab.state === "connecting"
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 10

                    Button {
                        visible: StreamTabManager.activeTab && StreamTabManager.activeTab.state === "error"
                        text: qsTr("Retry")
                        onClicked: if (StreamTabManager.activeTab) StreamTabManager.retryTab(StreamTabManager.activeTab.id)
                    }

                    Button {
                        text: qsTr("Close tab")
                        onClicked: if (StreamTabManager.activeTab) StreamTabManager.closeTab(StreamTabManager.activeTab.id)
                    }
                }
            }
        }
    }

    ErrorMessageDialog {
        id: noHwDecoderDialog
        text: Brand.text(qsTr("No functioning hardware accelerated video decoder was detected by Moonlight. " +
                              "Your streaming performance may be severely degraded in this configuration."))
        helpText: qsTr("Click the Help button for more information on solving this problem.")
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Fixing-Hardware-Decoding-Problems"
    }

    NavigableMessageDialog {
        id: portableUpdateDialog
        standardButtons: Dialog.NoButton
        closePolicy: Popup.CloseOnEscape
        showSpinner: true
        text: qsTr("Preparing update...")
    }

    BackgroundSourceDialog {
        id: backgroundSourceDialog

        onSourceChosen: function(source) {
            window.commitInitialBackgroundSource(source)
        }

        Connections {
            target: backgroundSourceDialog
            function onClosed() {
                // Escape and window-manager close mean “decide later”: keep the
                // photography default, mark the picker handled, then continue startup.
                if (!window.initialBackgroundChoiceHandled) {
                    window.commitInitialBackgroundSource(StreamingPreferences.BGS_PHOTOGRAPHY)
                }
                window.startConfigurationChecks()
            }
        }
    }

    ErrorMessageDialog {
        id: portableUpdateErrorDialog
        text: ""
    }

    ErrorMessageDialog {
        id: xWaylandDialog
        text: qsTr("Hardware acceleration doesn't work on XWayland. Continuing on XWayland may result in poor streaming performance. " +
                   "Try running with QT_QPA_PLATFORM=wayland or switch to X11.")
        helpText: qsTr("Click the Help button for more information.")
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Fixing-Hardware-Decoding-Problems"
    }

    NavigableMessageDialog {
        id: wow64Dialog
        standardButtons: Dialog.Ok | Dialog.Cancel
        text: Brand.text(qsTr("This version of Moonlight isn't optimized for your PC. Please download the '%1' version of Moonlight for the best streaming performance.")).arg(SystemProperties.friendlyNativeArchName)
        onAccepted: {
            Qt.openUrlExternally("https://github.com/moonlight-stream/moonlight-qt/releases");
        }
    }

    ErrorMessageDialog {
        id: unmappedGamepadDialog
        property string unmappedGamepads : ""
        text: Brand.text(qsTr("Moonlight detected gamepads without a mapping:")) + "\n" + unmappedGamepads
        helpTextSeparator: "\n\n"
        helpText: qsTr("Click the Help button for information on how to map your gamepads.")
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Gamepad-Mapping"
    }

    NavigableMessageDialog {
        id: quitConfirmationDialog
        standardButtons: Dialog.Yes | Dialog.No
        title: window.confirmWindowExit ? qsTr("Exit SunAndMon?") : ""
        text: window.confirmWindowExit && ComputerManager.center.hostActive
              ? qsTr("Exiting will stop local hosting and disconnect anyone connected to this computer.")
              : qsTr("Are you sure you want to quit?")
        onAccepted: {
            StreamTabManager.closeAllTabs()
            if (window.confirmWindowExit) {
                window.exitConfirmed = true
                window.close()
                window.exitConfirmed = false
            } else {
                Qt.quit()
            }
        }
    }

    // HACK: This belongs in StreamSegue but keeping a dialog around after the parent
    // dies can trigger bugs in Qt 5.12 that cause the app to crash. For now, we will
    // host this dialog in a QML component that is never destroyed.
    //
    // To repro: Start a stream, cut the network connection to trigger the "Connection
    // terminated" dialog, wait until the app grid times out back to the PC grid, then
    // try to dismiss the dialog.
    ErrorMessageDialog {
        id: streamSegueErrorDialog

        property bool quitAfter: false

        onClosed: {
            if (quitAfter) {
                Qt.quit()
            }

            // StreamSegue assumes its dialog will be re-created each time we
            // start streaming, so fake it by wiping out the text each time.
            text = ""
        }
    }

    GamepadKeyboard {
        id: gamepadKeyboard
    }

    NavigableDialog {
        id: addPcDialog
        property string label: qsTr("Enter the IP address of your host PC:")

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            editText.forceActiveFocus()
            // 手柄插拔状态以对话框打开那一刻为准
            oskHint.visible = SdlGamepadKeyNavigation.gamepadUiNavigationEnabled() &&
                              SdlGamepadKeyNavigation.getConnectedGamepads() > 0
        }

        onClosed: {
            editText.clear()
        }

        onAccepted: {
            const address = editText.text.trim()
            if (address.length > 0) {
                if (stackView.currentItem instanceof PcView)
                    stackView.currentItem.showLocalDevices()
                ComputerManager.addNewHostManually(address)
            }
        }

        ColumnLayout {
            spacing: Theme.spaceSm

            Text {
                text: addPcDialog.label
                color: Theme.text
                font.family: Theme.fontSans
                font.pointSize: Theme.fontRowTitle
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }

            HardTextField {
                id: editText
                readOnly: Qt.platform.os === "windows" && !addPcDialog.visible
                placeholderText: "192.168.1.100"
                Layout.fillWidth: true
                Layout.minimumWidth: 260
                focus: true

                Keys.onReturnPressed: {
                    addPcDialog.accept()
                }

                Keys.onEnterPressed: {
                    addPcDialog.accept()
                }
            }

            Text {
                id: oskHint
                visible: false
                text: qsTr("No keyboard? Press %1 to open the on-screen keyboard.").arg(SdlGamepadKeyNavigation.faceButtonGlyph(2))
                color: Theme.textFaint
                font.family: Theme.fontMono
                font.pointSize: Theme.fontBody
                Layout.fillWidth: true
            }

            // 云主机推广。放在这里是因为「我没有可以串流的主机」正好是打开这个框的
            // 人最可能卡住的地方 —— 手动填 IP 填不出一台主机来。
            //
            // 基地云目前只面向简体中文用户；自动语言模式下跟随系统语言。
            // 没有浏览器可用时也整块隐藏（和 QQ 按钮同一个判断），否则按钮点了没反应。
            Item {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceSm
                implicitHeight: promoColumn.implicitHeight
                visible: SystemProperties.hasBrowser &&
                         (StreamingPreferences.language === StreamingPreferences.LANG_ZH_CN ||
                          (StreamingPreferences.language === StreamingPreferences.LANG_AUTO &&
                           Qt.locale().name === "zh_CN"))

                Column {
                    id: promoColumn

                    anchors { left: parent.left; right: parent.right }
                    spacing: Theme.spaceSm

                    Rectangle {
                        width: parent.width
                        height: 1
                        color: Theme.line
                    }

                    MicroLabel {
                        width: parent.width
                        text: qsTr("No host PC of your own?")
                        // 这句比一般微标签长，允许折行（MicroLabel 默认单行省略）
                        elide: Text.ElideNone
                        wrapMode: Text.Wrap
                    }

                    Text {
                        width: parent.width
                        //: Procriva Cloud is a product name and must not be translated.
                        text: qsTr("Procriva Cloud rents out cloud hosts that are ready to stream.")
                        color: Theme.text
                        font.family: Theme.fontSans
                        font.pointSize: Theme.fontBody
                        wrapMode: Text.Wrap
                    }

                    HardButton {
                        text: qsTr("Learn more")
                        // 焦点默认停在输入框上，别让这颗按钮抢走
                        focusPolicy: Qt.TabFocus

                        onClicked: Qt.openUrlExternally("https://client.cloud.procriva.com/")
                    }
                }
            }
        }
    }
}
