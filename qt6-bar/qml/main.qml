import QtQuick
import QtQuick.Controls

// Floating taskbar, rendered as a layer-shell surface (top layer, full width).
// Visible/shown is controlled from C++ after the surface is configured,
// and toggled by the "window_full" event.
Window {
    id: win
    width: Screen.width
    height: barHeight          // context property, equals the exclusive zone (48)
    visible: false             // shown from C++ once the layer-shell surface exists
    color: "transparent"
    flags: Qt.FramelessWindowHint | Qt.WindowDoesNotAcceptFocus

    // ---------------------------------------------------------------- chrome
    Rectangle {
        anchors.fill: parent
        color: "#f2202020"     // Win11-ish dark taskbar, slightly translucent

        Rectangle {            // hairline top highlight
            width: parent.width
            height: 1
            anchors.top: parent.top
            color: "#26ffffff"
        }
        Rectangle {            // hairline bottom border
            width: parent.width
            height: 1
            anchors.bottom: parent.bottom
            color: "#33000000"
        }
    }

    // ---------------------------------------------------------------- content
    Row {
        anchors.fill: parent
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        topPadding: (parent.height - 40) / 2   // centre the 40px button row
        spacing: 2

        // ---- left icon: Windows-like logo, no function for now ----
        Item {
            width: 40
            height: 40

            Rectangle {                        // hover feedback only
                anchors.fill: parent
                radius: 4
                color: startMouse.containsMouse ? "#33ffffff" : "transparent"
            }
            Item {                             // 4-pane logo, drawn, no assets
                width: 22
                height: 22
                anchors.centerIn: parent
                Rectangle { x: 0;  y: 0;  width: 10; height: 10; color: "#F25022" }
                Rectangle { x: 12; y: 0;  width: 10; height: 10; color: "#7FBA00" }
                Rectangle { x: 0;  y: 12; width: 10; height: 10; color: "#00A4EF" }
                Rectangle { x: 12; y: 12; width: 10; height: 10; color: "#FFB900" }
            }
            MouseArea {
                id: startMouse
                anchors.fill: parent
                hoverEnabled: true
                // no action yet
            }
        }

        // ---- running windows (one icon per window) ----
        Repeater {
            model: bar.windows

            delegate: Item {
                id: taskItem
                width: 40
                height: 40

                readonly property bool focused: model.id === bar.focusedId

                // Win11-style focus background + hover highlight
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 2
                    radius: 4
                    color: taskItem.focused ? "#594d4d4d"
                                            : (itemMouse.containsMouse ? "#26ffffff" : "transparent")
                }

                // app icon from the theme
                Image {
                    id: iconImage
                    width: 24
                    height: 24
                    anchors.centerIn: parent
                    source: bar.findIcon(model.appId)
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

                // Win11 underline pill: bright when focused, faint otherwise
                Rectangle {
                    width: 14
                    height: 3
                    radius: 1.5
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 1
                    color: taskItem.focused ? "#d9ffffff" : "#66ffffff"
                }

                ToolTip {
                    text: model.appId + " (id " + model.id + ")"
                    visible: itemMouse.containsMouse
                    delay: 500
                }

                MouseArea {
                    id: itemMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: bar.activateWindow(model.id)
                }
            }
        }

        // ---- right side: connection indicator (green = socket up) ----
        Item {
            width: 14
            height: 40
            Rectangle {
                width: 8
                height: 8
                radius: 4
                anchors.centerIn: parent
                color: bar.connected ? "#7FBA00" : "#F25022"
                ToolTip.visible: statusMouse.containsMouse
                ToolTip.text: bar.connected ? "compositor connected" : "compositor disconnected"
            }
            MouseArea {
                id: statusMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: bar.debugMode = !bar.debugMode   // click the dot to toggle the debug panel
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
}
