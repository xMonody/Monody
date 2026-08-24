import QtQuick
import QtQuick.Layouts

// Running-window icons (one per window) + the right-click task context menu.
// Laid out as a Row so the parent RowLayout treats the whole group as one item;
// internal spacing matches the bar's app gap.
Row {
    id: root

    // The main taskbar window (id "win" in main.qml), used for the context
    // menu's geometry and for closing the other popups before opening.
    property var barWindow: null

    Layout.preferredHeight: barHeight
    spacing: barCfgAppGap        // distance between two app icons

    ColorUtils { id: taskbarColor }

    function closePopup() { contextMenu.closeMenu() }

    Repeater {
        model: bar.windows

        delegate: Item {
            id: taskItem
            width: 40
            height: barHeight

            readonly property bool focused: model.id === bar.focusedId
            readonly property int iconSize: 29

            // Liquid-glass focus/hover background (shared component):
            // translucent tint + light edge.
            // Same size in every state; only the tint depth changes.
            LiquidGlass {
                id: taskBg
                anchors.centerIn: parent
                // focus/hover pill: barCfgFocusPad extra on each side.
                // Height stays fixed (36) so the pill never grows taller
                // than the bar and its corners never get clipped.
                width: taskItem.iconSize + 2 * barCfgFocusPad
                height: 36
                lit: taskItem.focused
                hovered: itemMouse.containsMouse
            }

            // app icon from the theme (rendered by IconProvider, so
            // SVG icons survive Qt's weak built-in SVG renderer)
            Image {
                id: iconImage
                width: taskItem.iconSize
                height: taskItem.iconSize
                anchors.centerIn: parent
                source: "image://icons/" + encodeURIComponent(model.appId)
                sourceSize: Qt.size(64, 64)
                fillMode: Image.PreserveAspectFit
                smooth: true
                visible: status === Image.Ready

                // shrink while pressed, spring back on release
                scale: itemMouse.pressed ? 0.85 : 1.0
                Behavior on scale {
                    NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                }
            }

            // fallback tile when no themed icon was found
            Rectangle {
                visible: iconImage.status !== Image.Ready
                width: taskItem.iconSize
                height: taskItem.iconSize
                anchors.centerIn: parent
                radius: 5
                color: taskbarColor.hash(model.appId)

                // shrink while pressed, spring back on release
                scale: itemMouse.pressed ? 0.85 : 1.0
                Behavior on scale {
                    NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                }
                Text {
                    anchors.centerIn: parent
                    text: model.appId.charAt(0).toUpperCase()
                    color: "#ffffff"
                    font.pixelSize: 17
                    font.bold: true
                }
            }

            // (no tooltip: hovering an icon must not show the app name)

            MouseArea {
                id: itemMouse
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: (mouse) => {
                    if (mouse.button === Qt.RightButton) {
                        contextMenu.openFor(model.id, taskItem)
                    } else {
                        contextMenu.closeMenu()
                        // Windows behaviour: clicking a focused icon minimizes it
                        if (taskItem.focused)
                            bar.minimizeWindow(model.id)
                        else
                            bar.activateWindow(model.id)
                        if (root.barWindow)
                            root.barWindow.closeLauncher()
                    }
                }
            }
        }
    }

    // ---- task icon context menu: full-screen overlay window, see main.cpp ----
    //      Positioned near the cursor; any click outside the panel closes it
    //      (same architecture as the launcher).
    Window {
        id: contextMenu
        objectName: "contextMenuWindow"
        visible: false
        flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
        color: "transparent"

        width: Screen.width
        height: Screen.height

        // real logical size, see the launcher's note on fractional scales.
        // The bar window (win) is always configured by the compositor with
        // the true logical width, while this overlay's own width only settles
        // after its first show - so derive both from win.width.
        readonly property real trueWidth: root.barWindow.width
        readonly property real trueHeight: Screen.height * root.barWindow.width / Screen.width

        property int windowId: -1

        function openFor(id, anchor) {
            if (root.barWindow)
                root.barWindow.closeAllPopups()
            windowId = id
            // Windows-style jump-list placement: above the icon, horizontally
            // centered on it (below the icon when the bar sits at the top).
            // Coordinates are in this overlay's (global) space: the bar spans
            // the full width; at the bottom its top edge sits
            // (trueHeight - win.height) logical px from the screen top.
            var p = anchor.mapToItem(root.barWindow.contentItem, 0, 0)
            menuPanel.x = Math.round(p.x + (anchor.width - menuPanel.width) / 2)
            var gy = (barCfgAtTop ? 0 : trueHeight - root.barWindow.height) + p.y
            menuPanel.y = barCfgAtTop
                          ? Math.round(gy + anchor.height + 6)
                          : Math.round(gy - menuPanel.height - 6)
            // keep the panel fully on screen
            menuPanel.x = Math.max(4, Math.min(menuPanel.x,
                trueWidth - menuPanel.width - 4))
            menuPanel.y = Math.max(4, Math.min(menuPanel.y,
                trueHeight - menuPanel.height - 4))
            // Win11-style pop-in
            menuPanel.opacity = 0
            menuPanel.scale = 0.95
            menuPanel.visible = true
            contextMenu.visible = true
            menuAnim.start()
        }
        function closeMenu() { visible = false }

        // click-catcher: any click outside the panel closes the menu
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: contextMenu.closeMenu()
        }

        ParallelAnimation {
            id: menuAnim
            NumberAnimation { target: menuPanel; property: "opacity"; to: 1; duration: 140; easing.type: Easing.OutCubic }
            NumberAnimation { target: menuPanel; property: "scale"; to: 1; duration: 140; easing.type: Easing.OutCubic }
        }

        // Win11-style context menu panel
        Rectangle {
            id: menuPanel
            width: 150
            height: 3 * 34 + 2 * 2 + 8      // 3 items × 34 + spacing + margins
            radius: 8
            color: Qt.rgba(barCfgMenuBg.r, barCfgMenuBg.g, barCfgMenuBg.b, barCfgMenuBg.a)
            border.color: "#55666666"
            border.width: 1
            opacity: 0
            scale: 0.95

            Column {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 2

                Repeater {
                    model: [
                        { text: qsTr("关闭"), act: "close" },
                        { text: qsTr("最大化"), act: "max" },
                        { text: qsTr("最小化"), act: "min" }
                    ]
                    delegate: Rectangle {
                        width: menuPanel.width - 8
                        height: 34
                        radius: 5
                        color: menuItemMouse.containsMouse ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, barCfgActiveBg.a) : "transparent"
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.text
                            color: "#ffffff"
                            font.pixelSize: 13
                        }
                        MouseArea {
                            id: menuItemMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                var id = contextMenu.windowId
                                contextMenu.closeMenu()
                                if (modelData.act === "close")
                                    bar.closeWindow(id)
                                else if (modelData.act === "max")
                                    bar.maximizeWindow(id)
                                else
                                    bar.minimizeWindow(id)
                            }
                        }
                    }
                }
            }
        }
    }
}
