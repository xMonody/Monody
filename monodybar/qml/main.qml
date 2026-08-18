import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Floating taskbar, rendered as a layer-shell surface (top layer).
// Width comes from barCfgWidth (0 = full screen, see src/BarConfig.h).
// Visible/shown is controlled from C++ after the surface is configured,
// and toggled by the "window_full" event.
Window {
    id: win
    width: barCfgWidth > 0 ? barCfgWidth : Screen.width
    height: barHeight          // context property, equals the exclusive zone (38)
    visible: false             // shown from C++ once the layer-shell surface exists
    color: "transparent"
    flags: Qt.FramelessWindowHint | Qt.WindowDoesNotAcceptFocus

    // Corner radius of the taskbar body (0 = square corners, desktop shows through)
    property int barRadius: barCfgRadius

    // ---------------------------------------------------------------- chrome
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(barCfgBarColor.r, barCfgBarColor.g, barCfgBarColor.b, barCfgOpacity)
        radius: barRadius
        clip: true              // keep hairlines/content inside the rounded shape

        Rectangle {            // hairline top highlight (bottom edge when the bar is at the bottom)
            width: parent.width
            height: 1
            y: barCfgAtTop ? 0 : parent.height - 1
            color: "#26ffffff"
            topLeftRadius: barCfgAtTop ? barRadius : 0
            topRightRadius: barCfgAtTop ? barRadius : 0
            bottomLeftRadius: barCfgAtTop ? 0 : barRadius
            bottomRightRadius: barCfgAtTop ? 0 : barRadius
        }
        Rectangle {            // hairline bottom border (top edge when the bar is at the bottom)
            width: parent.width
            height: 1
            y: barCfgAtTop ? parent.height - 1 : 0
            color: "#33000000"
            bottomLeftRadius: barCfgAtTop ? barRadius : 0
            bottomRightRadius: barCfgAtTop ? barRadius : 0
            topLeftRadius: barCfgAtTop ? 0 : barRadius
            topRightRadius: barCfgAtTop ? 0 : barRadius
        }
    }

    // ---------------------------------------------------------------- content
    RowLayout {
        height: barHeight                      // button-row height, matches the bar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter   // centre the 40px row in the bar
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        spacing: 10

        // ---- left icon: Windows-like logo, no function for now ----
        Item {
            width: 40
            height: barHeight

            // liquid-glass hover/active background, same size as task icons
            LiquidGlass {
                anchors.centerIn: parent
                lit: launcher.visible              // lit while the start menu is open
                hovered: startMouse.containsMouse
            }
            Image {                            // Windows logo from win.png
                width: 24
                height: 24
                anchors.centerIn: parent
                source: "qrc:/win.png"
                sourceSize: Qt.size(120, 120)
                fillMode: Image.PreserveAspectFit
                smooth: true

                // shrink while pressed, spring back on release
                scale: startMouse.pressed ? 0.85 : 1.0
                Behavior on scale {
                    NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                }
            }
            MouseArea {
                id: startMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: launcher.visible ? closeLauncher() : openLauncher()   // start menu toggle
            }
        }

        // ---- gap: win icon stays two app-gap widths away from the apps ----
        Item { width: 10; height: barHeight }

        // ---- running windows (one icon per window) ----
        Repeater {
            model: bar.windows

            delegate: Item {
                id: taskItem
                width: 40
                height: barHeight

                readonly property bool focused: model.id === bar.focusedId

                // Liquid-glass focus/hover background (shared component):
                // translucent tint + specular sheen + fine noise + light edge.
                // Same size in every state; only the tint depth changes.
                LiquidGlass {
                    id: taskBg
                    anchors.centerIn: parent
                    lit: taskItem.focused
                    hovered: itemMouse.containsMouse
                }

                // app icon from the theme (rendered by IconProvider, so
                // SVG icons survive Qt's weak built-in SVG renderer)
                Image {
                    id: iconImage
                    width: 24
                    height: 24
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: -2
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
                    width: 24
                    height: 24
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: -2
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
                        font.pixelSize: 14
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
                            closeLauncher()
                        }
                    }
                }
            }
        }

        // ---- flexible spacer: keeps the tray + clock pinned right ----
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: barHeight
        }

        // ---- keyboard layout fallback (no tray items, e.g. fcitx5 absent) ----
        //      Windows-style input indicator: shows the active layout name.
        Item {
            implicitWidth: layoutText.implicitWidth + 16
            Layout.preferredHeight: barHeight
            visible: trayModel.count === 0

            LiquidGlass {
                anchors.centerIn: parent
                hovered: layoutMouse.containsMouse
            }
            Text {
                id: layoutText
                anchors.centerIn: parent
                text: bar.keyboardLayout.toUpperCase()
                color: "#d7dbe8"
                font.pixelSize: 12
                font.bold: true
            }
            MouseArea {
                id: layoutMouse
                anchors.fill: parent
                hoverEnabled: true
            }
        }

        // ---- system tray (StatusNotifier items: fcitx5, QQ, WeChat ...) ----
        Row {
            Layout.preferredHeight: barHeight
            spacing: 2
            visible: trayModel.count > 0

            Repeater {
                model: trayModel
                delegate: Item {
                    width: 32
                    height: barHeight

                    LiquidGlass {
                        anchors.centerIn: parent
                        lit: trayMouse.containsMouse        // stronger hover feedback
                        hovered: trayMouse.containsMouse
                    }
                    Image {
                        id: trayIcon
                        anchors.centerIn: parent
                        width: 24
                        height: 24
                        source: model.icon
                        sourceSize: Qt.size(48, 48)
                        fillMode: Image.PreserveAspectFit
                        smooth: true

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
                                contextMenu.trayOpenFor(index, trayIcon)
                            else
                                trayModel.activate(index)
                        }
                    }
                }
            }
        }

        // ---- clock: time above date, Win11 style (no seconds) ----
        Item {
            implicitWidth: clockRow.implicitWidth + 20
            Layout.preferredHeight: barHeight

            Rectangle {                        // hover feedback, same as icons
                anchors.fill: parent
                radius: 4
                color: clockMouse.containsMouse ? "#26ffffff" : "transparent"
            }
            Column {
                id: clockRow
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1
                width: Math.max(timeText.implicitWidth, dateText.implicitWidth)

                Text {
                    id: timeText
                    width: parent.width
                    text: Qt.formatTime(new Date(), "HH:mm")
                    color: "#626880"
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    horizontalAlignment: Text.AlignRight
                }
                Text {
                    id: dateText
                    width: parent.width
                    text: Qt.formatDate(new Date(), "yyyy/M/d")
                    color: "#626880"
                    font.pixelSize: 11
                    font.weight: Font.Bold
                    horizontalAlignment: Text.AlignRight
                }
            }
            MouseArea {
                id: clockMouse
                anchors.fill: parent
                hoverEnabled: true
            }
            Timer {
                interval: 1000
                running: true
                repeat: true
                onTriggered: {
                    timeText.text = Qt.formatTime(new Date(), "HH:mm")
                    dateText.text = Qt.formatDate(new Date(), "M/d/yyyy")
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
        readonly property real trueWidth: win.width
        readonly property real trueHeight: Screen.height * win.width / Screen.width

        property int windowId: -1

        function openFor(id, anchor) {
            windowId = id
            // Windows-style jump-list placement: above the icon, horizontally
            // centered on it (below the icon when the bar sits at the top).
            // Coordinates are in this overlay's (global) space: the bar spans
            // the full width; at the bottom its top edge sits
            // (trueHeight - win.height) logical px from the screen top.
            var p = anchor.mapToItem(win.contentItem, 0, 0)
            menuPanel.x = Math.round(p.x + (anchor.width - menuPanel.width) / 2)
            var gy = (barCfgAtTop ? 0 : contextMenu.trueHeight - win.height) + p.y
            menuPanel.y = barCfgAtTop
                          ? Math.round(gy + anchor.height + 6)
                          : Math.round(gy - menuPanel.height - 6)
            // keep the panel fully on screen
            menuPanel.x = Math.max(4, Math.min(menuPanel.x,
                contextMenu.trueWidth - menuPanel.width - 4))
            menuPanel.y = Math.max(4, Math.min(menuPanel.y,
                contextMenu.trueHeight - menuPanel.height - 4))
            // Win11-style pop-in
            menuPanel.opacity = 0
            menuPanel.scale = 0.95
            trayMenuPanel.visible = false
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
            color: Qt.rgba(barCfgBarColor.r, barCfgBarColor.g, barCfgBarColor.b, 0.96)
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
                        color: menuItemMouse.containsMouse ? "#26ffffff" : "transparent"
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

        // ---- tray icon context menu: the app's own menu via the SNI Menu
        //      property / com.canonical.dbusmenu protocol ----
        property int trayItemIndex: -1
        property var trayStack: []

        function trayOpenFor(index, anchor) {
            trayItemIndex = index
            trayStack = []
            if (!trayModel.fetchMenu(index))
                return
            trayShowItems(trayModel.menuItems(index, 0))
            var p = anchor.mapToItem(win.contentItem, 0, 0)
            trayMenuPanel.x = Math.round(p.x + (anchor.width - trayMenuPanel.width) / 2)
            var gy = (barCfgAtTop ? 0 : contextMenu.trueHeight - win.height) + p.y
            trayMenuPanel.y = barCfgAtTop
                          ? Math.round(gy + anchor.height + 6)
                          : Math.round(gy - trayMenuPanel.height - 6)
            trayMenuPanel.x = Math.max(4, Math.min(trayMenuPanel.x,
                contextMenu.trueWidth - trayMenuPanel.width - 4))
            trayMenuPanel.y = Math.max(4, Math.min(trayMenuPanel.y,
                contextMenu.trueHeight - trayMenuPanel.height - 4))
            trayMenuPanel.opacity = 0
            trayMenuPanel.scale = 0.95
            menuPanel.visible = false
            trayMenuPanel.visible = true
            contextMenu.visible = true
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
        function trayOpenSubmenu(parentId) {
            trayStack.push(parentId)
            trayShowItems(trayModel.menuItems(trayItemIndex, parentId))
            trayMenuPanel.y = Math.max(4, Math.min(trayMenuPanel.y,
                contextMenu.trueHeight - trayMenuPanel.height - 4))
        }
        function trayGoBack() {
            if (trayStack.length === 0)
                return
            trayStack.pop()
            var parent = trayStack.length > 0 ? trayStack[trayStack.length - 1] : 0
            trayShowItems(trayModel.menuItems(trayItemIndex, parent))
            trayMenuPanel.y = Math.max(4, Math.min(trayMenuPanel.y,
                contextMenu.trueHeight - trayMenuPanel.height - 4))
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
            color: Qt.rgba(barCfgBarColor.r, barCfgBarColor.g, barCfgBarColor.b, 0.96)
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
                            color: trayItemMouse.containsMouse && model.enabled ? "#26ffffff" : "transparent"
                        }
                        Text {
                            visible: model.type !== "separator"
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: model.label
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
                                if (model.isBack) { contextMenu.trayGoBack(); return }
                                if (model.hasChildren) { contextMenu.trayOpenSubmenu(model.id); return }
                                var idx = contextMenu.trayItemIndex
                                contextMenu.closeMenu()
                                trayModel.triggerMenu(idx, model.id)
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- launcher (start menu): full-screen layer surface, see main.cpp ----
    //      The panel sits under the win icon; a transparent click-catcher
    //      covers the rest of the screen so any outside click closes it.
    Window {
        id: launcher
        objectName: "launcherWindow"
        visible: false
        flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
        color: "transparent"

        width: Screen.width
        height: Screen.height

        // The compositor configures this overlay at the output's real
        // logical size.  With a fractional output scale (e.g. 1.75) Qt
        // rounds Screen.* up to the next integer scale (2.0), so Screen
        // reports the wrong logical size; the bar window's configured width
        // is authoritative, so re-derive the height from the same ratio.
        readonly property real trueWidth: win.width
        readonly property real trueHeight: Screen.height * win.width / Screen.width

        // 3 rows x 4 columns, 88x96px cells
        property int launcherCols: 4
        property int launcherRows: 3
        property int launcherCellW: 88
        property int launcherCellH: 96
        property int launcherPad: 10
        property bool hadFocus: false

        // close when the popup loses keyboard focus (after having had it)
        onActiveChanged: {
            if (active)
                hadFocus = true
            else if (hadFocus)
                closeLauncher()
        }
        onClosing: closeLauncher()

        // click-catcher: any click outside the panel (desktop, other windows,
        // the taskbar itself) closes the menu - works without focus events
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: closeLauncher()
        }

        Rectangle {                          // panel chrome: same bg as the taskbar
            // x follows the bar's left edge (bar is centred when fixed-width)
            x: (launcher.trueWidth - win.width) / 2 + 6
            // below the bar when at top, above it when at the bottom
            y: barCfgAtTop ? win.height + 6 : launcher.trueHeight - win.height - height - 6
            width: launcher.launcherCols * launcher.launcherCellW + 2 * launcher.launcherPad
            height: launcher.launcherRows * launcher.launcherCellH + 2 * launcher.launcherPad
            radius: win.barRadius
            color: Qt.rgba(barCfgBarColor.r, barCfgBarColor.g, barCfgBarColor.b, barCfgOpacity)
            border.color: "#55666666"
            border.width: 1

            GridView {
                id: appGrid
                anchors.fill: parent
                anchors.margins: launcher.launcherPad
                clip: true
                model: desktopApps
                cellWidth: launcher.launcherCellW
                cellHeight: launcher.launcherCellH
                focus: true
                Keys.onEscapePressed: closeLauncher()   // close the start menu

                ScrollBar.vertical: ScrollBar {   // thin scrollbar, only when needed
                    width: 3
                    policy: ScrollBar.AsNeeded
                    interactive: true
                    contentItem: Rectangle {
                        implicitWidth: 3
                        radius: 1.5
                        color: "#99ffffff"
                    }
                    background: Item {}
                }

                delegate: Item {
                    width: appGrid.cellWidth
                    height: appGrid.cellHeight

                    Rectangle {              // hover feedback: same as taskbar icons
                        anchors.fill: parent
                        anchors.margins: 2
                        radius: 4
                        color: appMouse.containsMouse ? "#26ffffff" : "transparent"
                    }

                    Column {
                        anchors.centerIn: parent
                        spacing: 5

                        Image {              // themed icon
                            id: appIcon
                            width: 44
                            height: 44
                            anchors.horizontalCenter: parent.horizontalCenter
                            source: model.icon
                            sourceSize: Qt.size(64, 64)
                            fillMode: Image.PreserveAspectFit
                            smooth: true
                            visible: status === Image.Ready
                        }
                        Rectangle {          // fallback tile when no icon was found
                            visible: appIcon.status !== Image.Ready
                            width: 44
                            height: 44
                            radius: 10
                            anchors.horizontalCenter: parent.horizontalCenter
                            color: taskbarColor.hash(model.name)
                            Text {
                                anchors.centerIn: parent
                                text: model.name.charAt(0).toUpperCase()
                                color: "#ffffff"
                                font.pixelSize: 18
                                font.bold: true
                            }
                        }
                        Text {               // app name
                            width: launcher.launcherCellW - 8
                            text: model.name
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            color: "#ffffff"
                            font.pixelSize: 11
                        }
                    }

                    MouseArea {
                        id: appMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            bar.launchApp(model.exec)   // start the app from Exec=
                            closeLauncher()
                        }
                    }
                }
            }
        }
    }

    // ---- debug panel (BAR_DEBUG=1 or click the status dot) ----
    Rectangle {
        visible: bar.debugMode
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 4
        width: debugText.implicitWidth + 16
        height: debugText.implicitHeight + 10
        radius: 5
        color: "#dd000000"
        border.color: "#66777777"
        border.width: 1
        z: 10
        Text {
            id: debugText
            anchors.centerIn: parent
            color: "#a8ffa8"
            font.family: "monospace"
            font.pixelSize: 11
            text: "socket: " + bar.socketPath + "\n" +
                  "conn:   " + (bar.connected ? "yes" : "no") + "\n" +
                  "wins:   " + bar.windowCount + "\n" +
                  "events: " + bar.eventsProcessed + "\n" +
                  "last:   " + bar.lastEvent + "\n" +
                  "focus:  " + (bar.focusedId < 0 ? "none" : "#" + bar.focusedId)
        }
    }

    // deterministic colour from a string, used for fallback tiles
    QtObject {
        id: taskbarColor
        function hash(s) {
            var h = 0
            for (var i = 0; i < s.length; i++)
                h = (h * 31 + s.charCodeAt(i)) % 360
            return Qt.hsla(h / 360, 0.45, 0.42, 1)
        }
    }

    // ---------------------------------------------------------------- helpers
    function openLauncher() {
        desktopApps.reload()              // pick up newly installed apps
        contextMenu.closeMenu()
        launcher.hadFocus = false
        launcher.visible = true
        launcher.requestActivate()
    }
    function closeLauncher() {
        // Reset hadFocus too: Qt may deliver the deactivate event caused by
        // this hide AFTER the user reopens the launcher, and a stale
        // hadFocus=true would make onActiveChanged close it again right away
        // (the "click twice to reopen" bug).
        launcher.hadFocus = false
        launcher.visible = false
    }

    // hide the launcher together with the bar (e.g. a window went fullscreen)
    onVisibleChanged: {
        if (!visible) {
            closeLauncher()
            contextMenu.closeMenu()
        }
    }
}
