import QtQuick

// Shared liquid-glass (frosted acrylic) background for bar buttons.
// Use via:
//     LiquidGlass { anchors.centerIn: parent; lit: <focused/active>; hovered: <mouse over> }
// lit = focused/selected window (deeper tint), hovered = light mouse-over tint.
Rectangle {
    id: glass

    property bool lit: false        // focused/active state (deeper tint)
    property bool hovered: false    // mouse-over state (light tint)

    width: 36
    height: 36
    radius: 6
    color: "transparent"
    antialiasing: true

    border.color: (lit || hovered) ? Qt.rgba(1, 1, 1, lit ? 0.30 : 0.18) : "transparent"
    border.width: 1

    // base tint: deeper for the focused window, light for hover
    Rectangle {
        anchors.fill: parent
        radius: parent.radius
        color: glass.lit
               ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, 0.40)
               : (glass.hovered ? Qt.rgba(1, 1, 1, 0.10) : "transparent")
    }

    // specular sheen: bright along the top edge, fading down
    Rectangle {
        anchors.fill: parent
        radius: parent.radius
        visible: glass.lit || glass.hovered
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, 0.20) }
            GradientStop { position: 0.45; color: Qt.rgba(1, 1, 1, 0.04) }
            GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.10) }
        }
    }

    // acrylic-style noise, drawn once and kept subtle
    Canvas {
        anchors.fill: parent
        visible: glass.lit || glass.hovered
        opacity: 0.10
        property bool drawn: false
        onPaint: {
            if (drawn) return
            drawn = true
            var ctx = getContext("2d")
            var w = width, h = height, r = 6
            for (var n = 0; n < 400; n++) {
                var x = Math.random() * w
                var y = Math.random() * h
                // keep dots inside the rounded shape
                var cx = Math.max(r - x, x - (w - 1 - r), 0)
                var cy = Math.max(r - y, y - (h - 1 - r), 0)
                if (cx * cx + cy * cy > r * r) continue
                var v = (255 * Math.random()).toFixed(0)
                ctx.fillStyle = "rgba(" + v + "," + v + "," + v + ",0.6)"
                ctx.fillRect(x, y, 1, 1)
            }
        }
        onVisibleChanged: if (visible) requestPaint()
    }
}
