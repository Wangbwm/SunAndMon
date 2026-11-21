pragma Singleton
import QtQuick 2.9
import StreamingPreferences 1.0

// 全应用的设计 token。风格是 neo-brutalism（新粗野主义）深色工业变体，
// 数值对齐参考站 https://client.cloud.procriva.com/ 的 --cend-* 变量。
//
// 配方只有六件事：方角、零模糊硬投影、极简中性色、单一荧光强调色、
// 几何变宽字体 + 等宽数字、宽字距大写微标签。想改风格只动这一个文件。
QtObject {
    readonly property bool modern: typeof sunAndMonBundle !== "undefined" && sunAndMonBundle
    readonly property bool dark: !modern || StreamingPreferences.darkAppearance
    readonly property color railSurface: modern && dark ? "#d5162233" : modern ? "#d8f7faff" : surface
    readonly property color glassSurface: modern && dark ? "#d41c283c" : modern ? "#dfffffff" : surfaceLayer
    readonly property color backdropBlue: dark ? "#25466e" : "#cbdffd"
    readonly property color backdropTeal: dark ? "#143b44" : "#d8eeec"
    readonly property color accentText: modern ? (dark ? "#0e243e" : "#ffffff") : ink
    readonly property color success: dark ? "#98e1c3" : "#17634e"
    readonly property color successSurface: dark ? "#233d38" : "#e4f4ed"
    readonly property int contentWidth: 1000
    // ---- 表面 ----
    // 一律不透明。壁纸只从卡片之间的缝隙里透出来，卡片本身要读起来像硬物件，
    // 不是叠在图上的一层玻璃 —— 这是和之前 glassmorphism 最根本的区别。
    readonly property color ink: modern ? (dark ? "#101927" : "#edf2f9") : "#0f1115"
    readonly property color surface: modern ? (dark ? "#1c283c" : "#ffffff") : "#171a20"
    readonly property color surface2: modern ? (dark ? "#233248" : "#eaf1fc") : "#1f232b"
    readonly property color surfaceLayer: modern ? surface : "#e0171a20"
    readonly property color surface2Layer: modern ? surface2 : "#e01f232b"
    readonly property color line: modern ? (dark ? "#394b63" : "#d8e3f2") : "#2b3038"
    readonly property color lineStrong: modern ? (dark ? "#647d9a" : "#93a8c0") : "#3c434e"

    // ---- 文字 ----
    readonly property color text: modern ? (dark ? "#eef5ff" : "#15283e") : "#eef0ec"
    readonly property color textDim: modern ? (dark ? "#a8bbd2" : "#496079") : "#aeb3ab"
    readonly property color textFaint: modern ? (dark ? "#8da3bd" : "#647b93") : "#7e858e"
    // 长说明比微标签更亮，避免在高 DPI 和复杂背景上退成一层灰雾。
    readonly property color textSettingsSubtitle: modern ? textDim : "#d7dbd5"

    // ---- 强调色 ----
    // 主色保留品牌青；酸性绿只留给「正在运行 / LIVE」这类抢眼状态标记，
    // 用得越少越有效，一旦到处都是就退化成普通的绿色装饰。
    readonly property color accent: modern ? (dark ? "#8bb9ff" : "#205ac9") : "#39c5bb"
    readonly property color accentStrong: modern ? (dark ? "#b3d1ff" : "#174bab") : "#5dd9d0"
    readonly property color accentDim: modern ? (dark ? "#72a5ef" : "#164299") : "#2ba39a"
    readonly property color accentSoft: modern ? (dark ? "#293f5f" : "#e4edfc") : "#2639c5bb"
    readonly property color acid: modern ? (dark ? "#93dcc1" : "#17634e") : "#c8ff4d"
    readonly property color acidGlow: modern ? (dark ? "#4993dcc1" : "#2917634e") : "#66c8ff4d"
    readonly property color danger: modern ? (dark ? "#ffa89b" : "#a33c30") : "#ff876f"

    // ---- 形状 ----
    // 全方角。参考站是 --cend-radius: 0 配上一堆 !important 去压 Naive UI 的圆角，
    // 我们这边对应的是逐控件覆盖 FluentWinUI3 的 background。
    readonly property int radiusCard: modern ? 16 : 0
    readonly property int radiusControl: modern ? 9 : 0

    // 零模糊、纯偏移的实心投影，对应 box-shadow: 6px 6px #0000008c。
    // QML 没有 box-shadow，Panel.qml 用一个偏移的实心矩形垫在本体后面模拟。
    readonly property int shadowOffset: modern ? 0 : 6
    readonly property int shadowOffsetLift: modern ? 0 : 9
    readonly property color shadowColor: "#8C000000"

    // border-left 粗条，参考站用 5px/9px 做强调，我们卡片用 4、状态标记用 5
    readonly property int accentBar: 4
    readonly property int accentBarStrong: 5

    // ---- 字体 ----
    // 打包在 app/res/fonts/，由 main.cpp 注册；中文靠系统字体回退。
    readonly property string fontSans: "Manrope"
    readonly property string fontMono: "DM Mono"

    // QML 的 font.letterSpacing 单位是像素，不是 em，所以要按字号折算。
    // 参考站的宽字距微标签是 .18em~.24em，标题是 -.03em。
    function tracking(pointSize, em) { return pointSize * 1.333 * em }
    function trackingWide(pointSize) { return tracking(pointSize, 0.2) }
    function trackingTight(pointSize) { return tracking(pointSize, -0.03) }
    // 常用字号的预折算值，绑定里直接用，省得到处调函数
    readonly property real trackingCaption: trackingWide(fontCaption)
    readonly property real trackingLabel: trackingWide(fontBody)

    // ---- 8pt 间距栅格 ----
    readonly property int spaceXs: 4
    readonly property int spaceSm: 8
    readonly property int spaceMd: 12
    readonly property int spaceLg: 16
    readonly property int spaceXl: 24

    // ---- 字号 ----
    readonly property int fontCardTitle: 13
    readonly property int fontHeroTitle: 18
    readonly property int fontRowTitle: 11
    readonly property int fontBody: 10
    readonly property int fontCaption: 9
    readonly property int fontSettingsSubtitle: fontBody

    // ---- 动效 ----
    // neo-brutalism 的动效更短更机械：不要弹、不要缓慢收尾，OutQuad 就够了。
    readonly property int durFast: 120
    readonly property int durNormal: 150
    readonly property int easing: Easing.OutQuad

    // ---- 布局 ----
    readonly property int railWidth: modern ? 220 : 208
    // 低于这个宽度时 rail 塌缩成顶部横向 tab
    readonly property int compactBreakpoint: 860
    // 低于这个内容宽度时，设置行把右侧控件移到说明文字下方。
    readonly property int settingsRowStackBreakpoint: 520
}
