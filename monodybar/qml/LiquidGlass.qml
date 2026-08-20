import QtQuick

// Shared liquid-glass (frosted acrylic) background for bar buttons.
// Use via:
//     LiquidGlass { anchors.centerIn: parent; lit: <focused/active>; hovered: <mouse over> }
// lit = focused/selected window (deeper tint), hovered = light mouse-over tint.
Rectangle {
    id: glass

    property bool lit: false        // focused/active state (deeper tint)
    property bool hovered: false    // mouse-over state (light tint)

    property int size: 36        // pill size, override for smaller icons
    implicitWidth: size
    implicitHeight: size
    radius: 6
    color: "transparent"
    antialiasing: true

    border.color: (lit || hovered) ? Qt.rgba(1, 1, 1, lit ? 0.10 : 0.05) : "transparent"
    border.width: barCfgActiveBorder     // 0 = no border (see BarConfig.h)

    // base tint: Win11-style soft translucent light pill - the dark taskbar
    // shows through it (deeper for the focused window, lighter on hover).
    // Not a solid box: the selection is transparent over the bar.
    Rectangle {
        anchors.fill: parent
        radius: parent.radius
        color: glass.lit
               ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b,
                         barCfgActiveBg.a)
               : (glass.hovered ? Qt.rgba(1, 1, 1, 0.07) : "transparent")
    }

}
