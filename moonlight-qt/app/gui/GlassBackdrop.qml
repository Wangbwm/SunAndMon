import QtQuick
import "theme"

Rectangle {
    id: backdrop

    property url wallpaper
    property real wallpaperStrength: 0
    readonly property bool softwareRendering: GraphicsInfo.api === GraphicsInfo.Software
    property bool blurEffectActive: false
    color: Theme.ink

    Canvas {
        id: lighting
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        readonly property color blue: Theme.backdropBlue
        readonly property color teal: Theme.backdropTeal
        onBlueChanged: requestPaint()
        onTealChanged: requestPaint()

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.clearRect(0, 0, width, height)

            function glow(x, y, radius, tint) {
                const gradient = ctx.createRadialGradient(x, y, 0, x, y, radius)
                gradient.addColorStop(0, tint)
                gradient.addColorStop(1, Qt.rgba(tint.r, tint.g, tint.b, 0))
                ctx.fillStyle = gradient
                ctx.fillRect(0, 0, width, height)
            }

            const radius = Math.max(width, height)
            glow(width * 0.78, height * 0.11, radius * 0.58, blue)
            glow(width * 0.47, height * 1.04, radius * 0.53, teal)
        }
    }

    Image {
        id: photograph
        anchors.fill: parent
        source: backdrop.wallpaperStrength > 0 ? backdrop.wallpaper : ""
        asynchronous: true
        fillMode: Image.PreserveAspectCrop
        visible: backdrop.softwareRendering || !backdrop.blurEffectActive
        opacity: visible ? backdrop.wallpaperStrength * 0.35 : 1
    }

    Item {
        id: blurHost
        anchors.fill: photograph

        Component.onCompleted: {
            if (backdrop.softwareRendering) {
                return
            }
            try {
                const effect = Qt.createQmlObject(
                    'import QtQuick; import QtQuick.Effects; MultiEffect {' +
                    '  anchors.fill: parent;' +
                    '  source: photograph;' +
                    '  visible: !backdrop.softwareRendering && photograph.status === Image.Ready && opacity > 0;' +
                    '  opacity: backdrop.wallpaperStrength;' +
                    '  autoPaddingEnabled: false;' +
                    '  blurEnabled: true;' +
                    '  blur: 0.9;' +
                    '  blurMax: 48;' +
                    '}',
                    blurHost,
                    "GlassBackdropMultiEffect"
                )
                if (effect) {
                    backdrop.blurEffectActive = true
                }
            } catch (e) {
                backdrop.blurEffectActive = false
            }
        }
    }
}
