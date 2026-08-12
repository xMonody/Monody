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

            Rectangle {                        // hover feedback only
                anchors.fill: parent
                radius: 4
                color: startMouse.containsMouse ? "#33ffffff" : "transparent"
            }
            Image {                            // Windows logo from win.png
                width: 24
                height: 24
                anchors.centerIn: parent
                source: "qrc:/win.png"
                sourceSize: Qt.size(120, 120)
                fillMode: Image.PreserveAspectFit
                smooth: true
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

                // Win11-style focus background + hover highlight.
                // Focused: a shorter rounded bar (narrower vertically) with
                // the underline pill inside; hover: full-height highlight.
                // The icon itself keeps its fixed size (never scaled).
                Rectangle {
                    id: taskBg
                    width: 36
                    height: taskItem.focused ? 30 : 36
                    anchors.centerIn: parent
                    radius: 4
                    color: (taskItem.focused && barCfgActiveBgEnabled) ? barCfgActiveBg
                                                                       : (itemMouse.containsMouse ? "#26ffffff" : "transparent")

                    // Win11 underline pill, inside the focused background
                    Rectangle {
                        visible: taskItem.focused
                        width: barCfgUnderlineWidth
                        height: barCfgUnderlineHeight
                        radius: Math.max(barCfgUnderlineHeight / 2, 1)
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: barCfgUnderlineOffset
                        color: barCfgUnderline
                    }
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
                    onClicked: {
                        bar.activateWindow(model.id)
                        closeLauncher()
                    }
                }
            }
        }

        // ---- flexible spacer: keeps the clock pinned to the far right ----
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: barHeight
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
        // reports the wrong logical size; the configured width (this
        // window's width) is authoritative, so re-derive the height from
        // the same ratio.
        readonly property real trueWidth: width
        readonly property real trueHeight: Screen.height * width / Screen.width

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
        launcher.hadFocus = false
        launcher.visible = true
        launcher.requestActivate()
    }
    function closeLauncher() {
        launcher.visible = false
    }

    // hide the launcher together with the bar (e.g. a window went fullscreen)
    onVisibleChanged: if (!visible) closeLauncher()
}
