import QtQuick
import "theme"

Item {
    id: curtain

    property bool allowed: true
    readonly property bool running: animation.running
    property real progress: 0
    property bool reversed: false
    property bool darkPalette: Theme.dark
    property var pendingChange: null

    visible: running
    Accessible.ignored: true

    function play(change, reverse, targetDark) {
        if (running) {
            return
        }
        if (!allowed || height < 1 || width < 1) {
            change()
            return
        }
        reversed = reverse
        darkPalette = targetDark === undefined ? Theme.dark : targetDark
        pendingChange = change
        progress = 0
        animation.start()
    }

    Canvas {
        id: water
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.clearRect(0, 0, width, height)

            const filling = curtain.progress <= 1
            const travel = filling ? curtain.progress : curtain.progress - 1
            const amplitude = Math.min(50, height / 8)
            const edge = (height + amplitude * 7) * travel - amplitude * 3.5
            const swing = Math.sin(travel * Math.PI * 1.7)
            const waveHeight = amplitude * (0.75 + 0.25 * swing)
            const liquid = ctx.createLinearGradient(0, 0, width, height)
            liquid.addColorStop(0, curtain.darkPalette ? "#111d2e" : "#f0f6ff")
            liquid.addColorStop(0.52, curtain.darkPalette ? "#234264" : "#cbdffd")
            liquid.addColorStop(1, curtain.darkPalette ? "#16484e" : "#cfebe9")

            if (curtain.reversed) {
                ctx.translate(width, 0)
                ctx.scale(-1, 1)
            }

            function fillWave(crest, fill) {
                ctx.beginPath()
                ctx.moveTo(0, crest + waveHeight)
                ctx.bezierCurveTo(width * 0.21, crest + waveHeight * 1.55,
                                  width * 0.29, crest - waveHeight * 1.45,
                                  width * 0.48, crest - waveHeight * 0.48)
                ctx.bezierCurveTo(width * 0.68, crest + waveHeight * 0.62,
                                  width * 0.82, crest + waveHeight * 1.35,
                                  width, crest - waveHeight * 0.25)
                ctx.lineTo(width, filling ? 0 : height)
                ctx.lineTo(0, filling ? 0 : height)
                ctx.closePath()
                ctx.fillStyle = fill
                ctx.fill()
            }

            const lip = Math.sin(travel * Math.PI) * amplitude * 0.65
            fillWave(edge + (filling ? lip : -lip), curtain.darkPalette ? "#496685" : "#f9fcff")
            fillWave(edge + (filling ? lip : -lip) * 0.47, curtain.darkPalette ? "#305879" : "#dbe8fc")
            fillWave(edge, liquid)
        }
    }

    onProgressChanged: water.requestPaint()
    onVisibleChanged: if (visible) water.requestPaint()

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: function(event) { event.accepted = true }
    }

    SequentialAnimation {
        id: animation

        NumberAnimation {
            target: curtain
            property: "progress"
            to: 1
            duration: 205
            easing.type: Easing.InOutSine
        }
        ScriptAction {
            script: {
                const change = curtain.pendingChange
                curtain.pendingChange = null
                change()
            }
        }
        NumberAnimation {
            target: curtain
            property: "progress"
            to: 2
            duration: 260
            easing.type: Easing.InOutSine
        }
    }
}
