import QtQuick
import QtQuick.Controls

// Start button (the Windows-like logo) + its start-menu overlay (launcher).
// Clicking the logo toggles the launcher; the launcher owns the whole screen
// while open so any outside click (handled by the click-catcher below) closes it.
Item {
    id: root

    // The main taskbar window (id "win" in main.qml), needed for the
    // launcher's geometry and for closing the other popups before opening.
    property var barWindow: null

    width: 40
    height: barHeight

    ColorUtils { id: taskbarColor }

    // liquid-glass hover/active background, same size as task icons
    LiquidGlass {
        anchors.centerIn: parent
        lit: launcher.visible              // lit while the start menu is open
        hovered: startMouse.containsMouse
    }
    Image {                            // Windows logo from win.png
        width: 26
        height: 26
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
        onClicked: launcher.visible ? closePopup() : openLauncher()   // start menu toggle
    }

    function openLauncher() {
        desktopApps.reload()              // pick up newly installed apps
        if (barWindow)
            barWindow.closeAllPopups()
        launcher.hadFocus = false
        launcher.visible = true
        launcher.requestActivate()
    }
    function closePopup() {
        // Reset hadFocus too: Qt may deliver the deactivate event caused by
        // this hide AFTER the user reopens the launcher, and a stale
        // hadFocus=true would make onActiveChanged close it again right away
        // (the "click twice to reopen" bug).
        launcher.hadFocus = false
        launcher.visible = false
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
        readonly property real trueWidth: root.barWindow.width
        readonly property real trueHeight: Screen.height * root.barWindow.width / Screen.width

        // 3 rows x 4 columns, 88x96px cells
        property int launcherCols: 4
        property int launcherRows: 3
        property int launcherCellW: 88
        property int launcherCellH: 96
        property int launcherPad: 10
        property int launcherFooterH: 56
        property bool hadFocus: false

        // close when the popup loses keyboard focus (after having had it)
        onActiveChanged: {
            if (active)
                hadFocus = true
            else if (hadFocus)
                root.closePopup()
        }
        onClosing: root.closePopup()

        // click-catcher: any click outside the panel (desktop, other windows,
        // the taskbar itself) closes the menu - works without focus events
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: root.closePopup()
        }

        Rectangle {                          // panel chrome: same bg as the popup menus (menuBg)
            // x follows the bar's left edge (bar is centred when fixed-width)
            x: (launcher.trueWidth - root.barWindow.width) / 2 + 6
            // below the bar when at top, above it when at the bottom
            y: barCfgAtTop ? root.barWindow.height + 6 : launcher.trueHeight - root.barWindow.height - height - 6
            width: launcher.launcherCols * launcher.launcherCellW + 2 * launcher.launcherPad
            height: launcher.launcherRows * launcher.launcherCellH + launcher.launcherFooterH + 3 * launcher.launcherPad
            radius: root.barWindow.barRadius
            color: Qt.rgba(barCfgMenuBg.r, barCfgMenuBg.g, barCfgMenuBg.b, barCfgMenuBg.a)
            border.color: "#55666666"
            border.width: 1

            GridView {
                id: appGrid
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: powerRow.top
                anchors.topMargin: launcher.launcherPad
                anchors.leftMargin: launcher.launcherPad
                anchors.rightMargin: launcher.launcherPad
                anchors.bottomMargin: launcher.launcherPad
                clip: true
                model: desktopApps
                cellWidth: launcher.launcherCellW
                cellHeight: launcher.launcherCellH
                focus: true
                Keys.onEscapePressed: root.closePopup()   // close the start menu

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
                            root.closePopup()
                        }
                    }
                }
            }

            // ---- footer: four power buttons (shutdown / restart / sleep /
            //      lock). Fixed at the panel bottom: they stay put while the
            //      app grid above scrolls. Icons come from icons/Power/.
            Row {
                id: powerRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: launcher.launcherPad
                anchors.rightMargin: launcher.launcherPad
                anchors.bottomMargin: launcher.launcherPad
                height: launcher.launcherFooterH
                spacing: 4

                Repeater {
                    id: powerRepeater
                    model: [
                        { action: "powerOff", icon: "qrc:/icons/Power/Shutdown.png", label: qsTr("关机") },
                        { action: "reboot",   icon: "qrc:/icons/Power/Restart.png", label: qsTr("重启") },
                        { action: "suspend",  icon: "qrc:/icons/Power/Sleep.png",   label: qsTr("睡眠") },
                        { action: "lock",     icon: "qrc:/icons/Power/Lock.png",    label: qsTr("锁定") }
                    ]

                    delegate: Rectangle {
                        id: powerBtn
                        width: (powerRow.width - powerRow.spacing * (powerRepeater.count - 1)) / powerRepeater.count
                        height: powerRow.height
                        radius: 6
                        color: powerMouse.containsMouse
                               ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, barCfgActiveBg.a)
                               : "transparent"
                        opacity: powerModule.available ? 1.0 : 0.4

                        Column {
                            anchors.centerIn: parent
                            spacing: 3

                            Image {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 22
                                height: 22
                                source: modelData.icon
                                sourceSize: Qt.size(48, 48)
                                fillMode: Image.PreserveAspectFit
                                smooth: true
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData.label
                                color: "#ffffff"
                                font.pixelSize: 11
                            }
                        }

                        MouseArea {
                            id: powerMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: powerModule.available
                            onClicked: {
                                var act = modelData.action
                                if (act === "powerOff")      powerModule.powerOff()
                                else if (act === "reboot")   powerModule.reboot()
                                else if (act === "suspend")  powerModule.suspend()
                                else if (act === "lock")     powerModule.lock()
                                root.closePopup()
                            }
                        }
                    }
                }
            }
        }
    }
}
