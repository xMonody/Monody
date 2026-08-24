import QtQuick
import QtQuick.Layouts

// System tray (StatusNotifier items: fcitx5, QQ, WeChat ...) + the tray
// icon's right-click menu (the app's own menu via the SNI Menu property /
// com.canonical.dbusmenu protocol).
Row {
    id: root

    // The main taskbar window (id "win" in main.qml).
    property var barWindow: null

    Layout.preferredHeight: barHeight
    spacing: 2
    visible: trayModel.count > 0

    function closePopup() { trayMenu.closeMenu() }

    Repeater {
        model: trayModel
        delegate: Item {
            width: 32
            height: barHeight

            // hover background: same pill as the app icons
            LiquidGlass {
                anchors.centerIn: parent
                size: 32
                hovered: trayMouse.containsMouse
            }
            Image {
                id: trayIcon
                anchors.centerIn: parent
                width: 19
                height: 19
                source: model.icon
                sourceSize: Qt.size(48, 48)
                fillMode: Image.PreserveAspectFit
                smooth: true

                // shrink 3 px while pressed (19 -> 16), spring back
                // on release
                scale: trayMouse.pressed ? 16.0 / 19.0 : 1.0
                Behavior on scale {
                    NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                }

                // Message notification: gentle 1 Hz blink while the
                // item wants attention (brief dim, mostly bright).
                // opacity is a plain binding - the animation drives
                // blinkOpacity, not opacity itself - so the icon
                // snaps back to fully opaque the instant attention
                // ends, even mid-fade (no stuck grey icon).
                property real blinkOpacity: 1.0
                opacity: model.attention ? blinkOpacity : 1.0

                SequentialAnimation {
                    id: blinkAnim
                    running: model.attention
                    loops: Animation.Infinite
                    NumberAnimation {
                        target: trayIcon
                        property: "blinkOpacity"
                        to: 0.15; duration: 120
                        easing.type: Easing.OutQuad
                    }
                    NumberAnimation {
                        target: trayIcon
                        property: "blinkOpacity"
                        to: 1.0; duration: 880
                        easing.type: Easing.InQuad
                    }
                }
            }
            MouseArea {
                id: trayMouse
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: (mouse) => {
                    // Left click activates the app; right click shows
                    // the app's own menu via the dbusmenu protocol.
                    if (mouse.button === Qt.RightButton)
                        trayMenu.trayOpenFor(index, trayIcon)
                    else
                        trayModel.activate(index)
                }
            }
        }
    }

    // ---- tray icon context menu: the app's own menu via the SNI Menu
    //      property / com.canonical.dbusmenu protocol ----
    Window {
        id: trayMenu
        objectName: "trayMenuWindow"
        visible: false
        flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
        color: "transparent"

        width: Screen.width
        height: Screen.height

        readonly property real trueWidth: root.barWindow.width
        readonly property real trueHeight: Screen.height * root.barWindow.width / Screen.width

        property int trayItemIndex: -1
        property var trayStack: []

        function trayOpenFor(index, anchor) {
            if (root.barWindow)
                root.barWindow.closeAllPopups()
            trayItemIndex = index
            trayStack = []
            if (!trayModel.fetchMenu(index))
                return
            trayShowItems(trayModel.menuItems(index, 0))
            var p = anchor.mapToItem(root.barWindow.contentItem, 0, 0)
            trayMenuPanel.x = Math.round(p.x + (anchor.width - trayMenuPanel.width) / 2)
            positionTrayMenu()
            trayMenuPanel.opacity = 0
            trayMenuPanel.scale = 0.95
            trayMenuPanel.visible = true
            trayMenu.visible = true
            trayMenuAnim.start()
        }
        function trayShowItems(items) {
            trayListModel.clear()
            if (trayStack.length > 0)
                trayListModel.append({ id: -1, label: qsTr("← 返回"), enabled: true,
                                       type: "", hasChildren: false, isBack: true })
            for (var i = 0; i < items.length; i++)
                trayListModel.append(items[i])
        }
        function positionTrayMenu() {
            // Anchor to the bar edge (6 px gap) using the panel's CURRENT
            // height, then clamp into the screen.  Must run after every
            // content change: trayOpenSubmenu/trayGoBack change the item
            // count, so a stale y would otherwise leave the panel over the
            // bar or off-screen.
            trayMenuPanel.y = barCfgAtTop
                ? Math.round(root.barWindow.height + 6)
                : Math.round(trueHeight - root.barWindow.height - trayMenuPanel.height - 6)
            trayMenuPanel.y = Math.max(4, Math.min(trayMenuPanel.y,
                trueHeight - trayMenuPanel.height - 4))
            trayMenuPanel.x = Math.max(4, Math.min(trayMenuPanel.x,
                trueWidth - trayMenuPanel.width - 4))
        }
        function trayOpenSubmenu(parentId) {
            trayStack.push(parentId)
            trayShowItems(trayModel.menuItems(trayItemIndex, parentId))
            positionTrayMenu()
        }
        function trayGoBack() {
            if (trayStack.length === 0)
                return
            trayStack.pop()
            var parent = trayStack.length > 0 ? trayStack[trayStack.length - 1] : 0
            trayShowItems(trayModel.menuItems(trayItemIndex, parent))
            positionTrayMenu()
        }
        function closeMenu() { visible = false }

        // click-catcher: any click outside the panel closes the menu
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: trayMenu.closeMenu()
        }

        ListModel { id: trayListModel }

        ParallelAnimation {
            id: trayMenuAnim
            NumberAnimation { target: trayMenuPanel; property: "opacity"; to: 1; duration: 140; easing.type: Easing.OutCubic }
            NumberAnimation { target: trayMenuPanel; property: "scale"; to: 1; duration: 140; easing.type: Easing.OutCubic }
        }

        Rectangle {
            id: trayMenuPanel
            visible: false
            width: 180
            height: trayListModel.count * 34 + 2 * Math.max(0, trayListModel.count - 1) + 8
            radius: 8
            color: Qt.rgba(barCfgMenuBg.r, barCfgMenuBg.g, barCfgMenuBg.b, barCfgMenuBg.a)
            border.color: "#55666666"
            border.width: 1
            opacity: 0
            scale: 0.95
            z: 2

            Column {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 2

                Repeater {
                    model: trayListModel
                    delegate: Item {
                        width: trayMenuPanel.width - 8
                        height: 34

                        Rectangle {
                            visible: model.type === "separator"
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            height: 1
                            color: "#33ffffff"
                        }
                        Rectangle {
                            visible: model.type !== "separator"
                            anchors.fill: parent
                            radius: 5
                            color: trayItemMouse.containsMouse && model.enabled ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, barCfgActiveBg.a) : "transparent"
                        }
                        Text {
                            visible: model.type !== "separator"
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.right: parent.right
                            // leave room for the "›" chevron on submenu items
                            anchors.rightMargin: model.type !== "separator" && model.hasChildren ? 28 : 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: model.label
                            elide: Text.ElideRight
                            color: model.enabled ? "#ffffff" : "#66ffffff"
                            font.pixelSize: 13
                        }
                        Text {
                            visible: model.type !== "separator" && model.hasChildren
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: "›"
                            color: "#aaffffff"
                            font.pixelSize: 16
                        }
                        MouseArea {
                            id: trayItemMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: {
                                if (model.type === "separator") return
                                if (model.id === -1 || model.isBack) { trayMenu.trayGoBack(); return }
                                if (model.hasChildren) { trayMenu.trayOpenSubmenu(model.id); return }
                                var idx = trayMenu.trayItemIndex
                                // close only when the item actually triggered:
                                // a failed Event keeps the menu open so the
                                // user can retry instead of it "dying"
                                if (trayModel.triggerMenu(idx, model.id))
                                    trayMenu.closeMenu()
                            }
                        }
                    }
                }
            }
        }
    }
}
