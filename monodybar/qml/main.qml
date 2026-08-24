import QtQuick
import QtQuick.Layouts

// Floating taskbar, rendered as a layer-shell surface (top layer).
// Width comes from barCfgWidth (0 = full screen, see src/BarConfig.h).
// Visible/shown is controlled from C++ after the surface is configured,
// and toggled by the "window_full" event.
//
// The bar content is split across several component files:
//   WinStart.qml   - start button + start-menu popup
//   AppIcons.qml   - running-window icons + task context menu
//   Tray.qml       - system tray + tray menu
//   StatusBar.qml  - volume/battery/network/bluetooth + clock + quick settings
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
        spacing: barCfgAppGap        // distance between two app icons

        // ---- start button + start-menu popup ----
        WinStart {
            id: winStart
            barWindow: win
        }

        // ---- gap: extra width so the win icon sits barCfgWinAppGap px from
        //      the first app (RowLayout spacing already adds appGap per side) ----
        Item { width: Math.max(0, barCfgWinAppGap - 2 * barCfgAppGap); height: barHeight }

        // ---- running windows (one icon per window) + task context menu ----
        AppIcons {
            id: appIcons
            barWindow: win
        }

        // ---- flexible spacer: keeps the tray + clock pinned right ----
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: barHeight
        }

        // ---- tray + status + clock, grouped so the gap on both sides of the
        //      status pill stays symmetric (barCfgStatusGap) ----------------
        Row {
            Layout.preferredHeight: barHeight
            spacing: barCfgStatusGap

            // ---- system tray + tray menu ----------------
            Tray {
                id: tray
                barWindow: win
            }

            // ---- status (volume/battery/network/bluetooth) + clock + quick settings ----
            StatusBar {
                id: statusBar
                barWindow: win
            }
        }
    }

    // Popup coordination: the popups are full-screen overlays, so only one
    // should be visible at a time.  Each component calls closeAllPopups()
    // before opening its own, and closeLauncher() when a task icon is clicked.
    function closeAllPopups() {
        if (winStart) winStart.closePopup()
        if (appIcons) appIcons.closePopup()
        if (tray) tray.closePopup()
        if (statusBar) statusBar.closePopup()
    }
    function closeLauncher() {
        if (winStart) winStart.closePopup()
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

    // Win11-style toggle switch used in the quick-settings panel
    component ToggleSwitch: Item {
        property bool on: false

        width: 36
        height: 20

        Rectangle {   // track
            anchors.fill: parent
            radius: height / 2
            color: parent.on ? "#3a9b5f" : "#5a5a5a"
            Behavior on color { ColorAnimation { duration: 120 } }
        }
        Rectangle {   // knob
            width: 16
            height: 16
            radius: 8
            color: "#eeeeee"
            x: parent.on ? parent.width - width - 2 : 2
            y: (parent.height - height) / 2
            Behavior on x { NumberAnimation { duration: 120 } }
        }
    }

    // hide the launcher and any open popup together with the bar
    // (e.g. a window went fullscreen)
    onVisibleChanged: {
        if (!visible)
            closeAllPopups()
    }
}
