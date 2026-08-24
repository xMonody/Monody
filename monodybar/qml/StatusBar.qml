import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// System status (bluetooth / network / battery / volume) + the clock, and the
// quick-settings popup opened by the status pill.
//
// The status pill is ONE button: clicking it opens the quick-settings panel.
// The pill sits next to the clock with the same symmetric gap
// (barCfgStatusGap) used between the tray and the pill in main.qml.
Row {
    id: root

    // The main taskbar window (id "win" in main.qml).
    property var barWindow: null

    Layout.preferredHeight: barHeight
    Layout.rightMargin: 0
    spacing: barCfgStatusGap

    function openPanel() {
        if (barWindow)
            barWindow.closeAllPopups()
        quickSettings.positionQuick(statusPanel)
    }
    function closePopup() { quickSettings.visible = false }

    // battery icon by level + charging state (matches icons/Battery/*.png;
    // note the missing "g" in the 0-15 charging filename)
    function batteryImage(percent, charging) {
        var range = percent <= 15 ? "0-15"
                  : percent <= 30 ? "15-30"
                  : percent <= 45 ? "30-45"
                  : percent <= 60 ? "45-60"
                  : percent <= 75 ? "60-75"
                  : percent <= 90 ? "75-90"
                  : "90-100"
        if (charging && percent <= 15)
            return "qrc:/icons/Battery/BatteryChargin0-15.png"
        return charging
                ? "qrc:/icons/Battery/BatteryCharging" + range + ".png"
                : "qrc:/icons/Battery/Battery" + range + ".png"
    }

    // volume icon by level + mute state (matches icons/Volume/*.png)
    function volumeImage(percent, muted) {
        if (muted || percent <= 0)
            return "qrc:/icons/Volume/Volume0.png"
        if (percent < 33)
            return "qrc:/icons/Volume/Volume1-33.png"
        if (percent < 66)
            return "qrc:/icons/Volume/Volume33-66.png"
        return "qrc:/icons/Volume/Volume66-100.png"
    }

    // ---- system status: bluetooth / network / battery (D-Bus modules) ----
    //      The whole pill is ONE button: click opens the quick-settings
    //      popup (quickSettings.statusPanel).  No per-icon clicks.
    Item {
        height: barHeight
        // implicitWidth (not width): the Row sizes us from this,
        // so the single pill spans the whole group like one button
        implicitWidth: statusRow.width + 2 * barCfgFocusPad

        visible: bluetoothModule.available
                 || networkModule.available
                 || (batteryModule.available && batteryModule.devicePath !== "")
                 || (volumeModule.available && volumeModule.sinkName !== "")

        // click the whole button -> quick-settings popup
        MouseArea {
            id: statusMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.openPanel()
        }

        // one liquid-glass pill for the whole group
        LiquidGlass {
            anchors.centerIn: parent
            width: parent.width
            height: 36
            hovered: statusMouse.containsMouse
        }

        Row {
            id: statusRow
            anchors.centerIn: parent
            spacing: 2

            // ---- volume (PipeWire): icon + fixed-width percent ----
            Item {
                id: volumeItem
                width: barCfgShowStatusPercent
                       ? 2 + volumeIcon.width + 2 + volumePct.width + 2
                       : 2 + volumeIcon.width + 2
                height: barHeight
                visible: volumeModule.available && volumeModule.sinkName !== ""

                Image {
                    id: volumeIcon
                    width: 20
                    height: 20
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 2
                    // icon by level; dimmed when muted
                    source: root.volumeImage(volumeModule.percent, volumeModule.muted)
                    opacity: volumeModule.muted ? 0.55 : 1.0
                    sourceSize: Qt.size(32, 32)
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                }
                Text {
                    id: volumePct
                    visible: barCfgShowStatusPercent
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: volumeIcon.right
                    anchors.leftMargin: 2
                    width: 30
                    height: parent.height
                    horizontalAlignment: Text.AlignLeft
                    verticalAlignment: Text.AlignVCenter
                    // space-padded to 3 digits: 1 -> "  1", 20 -> " 20", 100 -> "100"
                    text: ("   " + volumeModule.percent).slice(-3) + "%"
                    color: volumeModule.muted ? "#777777" : "#999999"
                    font.pixelSize: 12
                    font.weight: Font.Bold
                }
            }

            // ---- bluetooth (org.bluez): indicator only, no click ----
            Item {
                id: btItem
                width: 32
                height: barHeight
                visible: bluetoothModule.available

                Image {
                    anchors.centerIn: parent
                    width: 20
                    height: 20
                    // connected icon when something is paired/connected
                    source: bluetoothModule.connectedCount > 0
                            ? "qrc:/icons/Bluetooth/Bluetoothcon.png"
                            : "qrc:/icons/Bluetooth/Bluetoothon.png"
                    // dim the icon when the adapter is powered off
                    opacity: bluetoothModule.powered ? 1.0 : 0.35
                    sourceSize: Qt.size(32, 32)
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                }
            }

            // ---- network (org.freedesktop.NetworkManager): indicator only ----
            Item {
                id: netItem
                width: 32
                height: barHeight
                visible: networkModule.available

                Image {
                    anchors.centerIn: parent
                    width: 20
                    height: 20
                    source: networkModule.type === "wired"
                            ? "qrc:/icons/network/WiredConnection.png"
                            : networkModule.connected
                              ? "qrc:/icons/network/SconnectWifi.png"
                              : "qrc:/icons/network/DisconnectWifi.png"
                    opacity: networkModule.connected ? 1.0 : 0.45
                    sourceSize: Qt.size(32, 32)
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                }
            }

            // ---- battery (org.freedesktop.UPower): icon + percent ----
            Item {
                id: batteryItem
                width: barCfgShowStatusPercent
                       ? 2 + batteryIcon.width + 2 + batteryPct.width + 2
                       : 2 + batteryIcon.width + 2
                height: barHeight
                visible: batteryModule.available && batteryModule.devicePath !== ""

                Image {
                    id: batteryIcon
                    width: 21
                    height: 21
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 2
                    source: root.batteryImage(batteryModule.percent, batteryModule.charging)
                    sourceSize: Qt.size(32, 32)
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                }
                Text {
                    id: batteryPct
                    visible: barCfgShowStatusPercent
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: batteryIcon.right
                    anchors.leftMargin: 2
                    width: 30
                    height: parent.height
                    horizontalAlignment: Text.AlignLeft
                    verticalAlignment: Text.AlignVCenter
                    text: ("   " + batteryModule.percent).slice(-3) + "%"
                    color: "#999999"
                    font.pixelSize: 12
                    font.weight: Font.Bold
                }
            }
        }

        // press shrinks the whole button, same spring as the clock
        scale: statusMouse.pressed ? 0.9 : 1.0
        Behavior on scale {
            SpringAnimation {
                spring: 2.0
                damping: 0.35
                mass: 1.0
            }
        }
    }

    // ---- clock: time only, press-to-shrink (no hover zoom) ----
    Item {
        id: clockItem
        implicitWidth: clockRow.width + 2 * barCfgFocusPad
        height: barHeight

        // hover feedback: same liquid-glass pill (tint + border) as the
        // taskbar app icons, incl. border color/opacity (LiquidGlass.qml)
        LiquidGlass {
            id: clockBg
            anchors.centerIn: parent
            width: clockRow.width + 2 * barCfgFocusPad
            height: 36
            hovered: clockMouse.containsMouse
        }

        // time only (no icon)
        Item {
            id: clockRow
            anchors.centerIn: parent
            width: timeText.implicitWidth
            height: parent.height

            Text {
                id: timeText
                anchors.verticalCenter: parent.verticalCenter
                text: Qt.formatTime(new Date(), "HH:mm")
                color: "#999999"
                font.pixelSize: 16
                font.weight: Font.Bold
            }
        }

        MouseArea {
            id: clockMouse
            anchors.fill: parent
            hoverEnabled: true
        }

        // press shrinks the clock; no hover animation
        scale: clockMouse.pressed ? 0.9 : 1.0
        Behavior on scale {
            SpringAnimation {
                spring: 2.0
                damping: 0.35
                mass: 1.0
            }
        }

        Timer {
            interval: 1000
            running: true
            repeat: true
            onTriggered: timeText.text = Qt.formatTime(new Date(), "HH:mm")
        }
    }

    // ---- quick-settings popup: full-screen overlay window, see main.cpp ----
    //      Win11-style: two big buttons (wifi / bluetooth). Left half of a
    //      button toggles it on/off, the chevron on the right opens the
    //      scan + device-list sub-panel.  Buttons are disabled when the
    //      service/hardware is missing.
    Window {
        id: quickSettings
        objectName: "quickSettingsWindow"
        visible: false
        flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
        color: "transparent"

        width: Screen.width
        height: Screen.height

        readonly property real trueWidth: root.barWindow.width
        readonly property real trueHeight: Screen.height * root.barWindow.width / Screen.width

        property bool wifiQuickEnabled: networkModule.available
                                       && networkModule.hasWifiDevice
        property bool btQuickEnabled: bluetoothModule.available
                                     && bluetoothModule.hasAdapter

        function positionQuick(panel) {
            panel.y = barCfgAtTop
                ? Math.round(root.barWindow.height + 6)
                : Math.round(trueHeight - root.barWindow.height - panel.height - 6)
            panel.x = Math.round(trueWidth - panel.width - 8)
            statusPanel.visible = (panel === statusPanel)
            wifiSubPanel.visible = (panel === wifiSubPanel)
            btSubPanel.visible = (panel === btSubPanel)
            panel.opacity = 0
            panel.scale = 0.95
            quickOp.target = panel
            quickSc.target = panel
            quickSettings.visible = true
            quickAnim.start()
        }
        // re-anchor the quick-settings panel whenever its height changes
        // (rows appear/disappear as modules connect): a stale y would let
        // the panel's bottom edge slide down over the status bar.
        function reAnchorQuick(panel) {
            var th = trueHeight
            var tw = trueWidth
            if (!(th > 0) || !isFinite(th)) th = Screen.height
            if (!(tw > 0) || !isFinite(tw)) tw = Screen.width
            panel.y = barCfgAtTop
                ? Math.round(root.barWindow.height + 6)
                : Math.round(th - root.barWindow.height - panel.height - 6)
            panel.y = Math.max(4, Math.min(panel.y,
                th - (barCfgAtTop ? 0 : root.barWindow.height) - panel.height - 4))
            panel.x = Math.max(4, Math.min(Math.round(tw - panel.width - 8),
                                           tw - panel.width - 4))
        }
        function wifiSubOpen() { positionQuick(wifiSubPanel) }
        function btSubOpen() { positionQuick(btSubPanel) }
        function quickBack() { positionQuick(statusPanel) }
        function closeMenu() { visible = false }

        // click-catcher: any click outside the panel closes the popup
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: quickSettings.closeMenu()
        }

        ParallelAnimation {
            id: quickAnim
            NumberAnimation { id: quickOp; property: "opacity"; to: 1; duration: 140; easing.type: Easing.OutCubic }
            NumberAnimation { id: quickSc; property: "scale"; to: 1; duration: 140; easing.type: Easing.OutCubic }
        }

        // ---- main quick-settings panel ----
        Rectangle {
            id: statusPanel
            visible: false
            width: 280
            height: statusCol.implicitHeight + 8
            // rows appear/disappear as modules connect: keep the panel
            // anchored above the bar instead of growing down over it
            onHeightChanged: if (visible) quickSettings.reAnchorQuick(statusPanel)
            radius: 8
            color: Qt.rgba(barCfgMenuBg.r, barCfgMenuBg.g, barCfgMenuBg.b, barCfgMenuBg.a)
            border.color: "#55666666"
            border.width: 1
            opacity: 0
            scale: 0.95
            z: 3

            // absorb clicks on panel gaps so they don't close the popup
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
            }

            Column {
                id: statusCol
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4

                // ---- wifi button ----
                Rectangle {
                    id: wifiBtn
                    width: parent.width
                    height: 52
                    radius: 8
                    color: (wifiLeft.containsMouse || wifiChevron.containsMouse)
                           ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, barCfgActiveBg.a)
                           : "transparent"
                    opacity: quickSettings.wifiQuickEnabled ? 1.0 : 0.45

                    // left half: toggle the wifi radio
                    MouseArea {
                        id: wifiLeft
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: wifiChevron.left
                        hoverEnabled: true
                        enabled: quickSettings.wifiQuickEnabled
                        onClicked: networkModule.setWirelessEnabled(!networkModule.wirelessEnabled)
                    }
                    // right chevron: open the scan/list sub-panel
                    MouseArea {
                        id: wifiChevron
                        width: 46
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.right: parent.right
                        hoverEnabled: true
                        enabled: quickSettings.wifiQuickEnabled
                        onClicked: quickSettings.wifiSubOpen()

                        // hover highlight: make the "click to enter"
                        // affordance visible on the right edge
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 6
                            radius: 6
                            color: parent.containsMouse
                                   ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, barCfgActiveBg.a)
                                   : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: "›"
                                color: "#99ffffff"
                                font.pixelSize: 22
                            }
                        }
                    }

                    Image {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        width: 22
                        height: 22
                        source: networkModule.type === "wired"
                                ? "qrc:/icons/network/WiredConnection.png"
                                : (networkModule.hasWifiDevice && !networkModule.connected)
                                  ? "qrc:/icons/network/DisconnectWifi.png"
                                  : "qrc:/icons/network/SconnectWifi.png"
                        sourceSize: Qt.size(32, 32)
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                    }
                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 44
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text {
                            text: networkModule.type === "wired"
                                  ? qsTr("有线网络")
                                  : !networkModule.hasWifiDevice
                                    ? qsTr("无设备")
                                    : qsTr("无线网络")
                            color: "#ffffff"
                            font.pixelSize: 14
                        }
                        Text {
                            text: networkModule.type === "wired"
                                  ? (networkModule.name.length > 0
                                     ? networkModule.name
                                     : qsTr("已连接"))
                                  : !networkModule.hasWifiDevice
                                    ? qsTr("未检测到 Wi-Fi 设备")
                                    : networkModule.connected
                                      ? (networkModule.name.length > 0
                                         ? networkModule.name
                                         : qsTr("网络已连接"))
                                      : qsTr("未连接")
                            color: "#99ffffff"
                            font.pixelSize: 11
                        }
                    }
                }

                // ---- bluetooth button ----
                Rectangle {
                    id: btBtn
                    width: parent.width
                    height: 52
                    radius: 8
                    color: (btLeft.containsMouse || btChevron.containsMouse)
                           ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, barCfgActiveBg.a)
                           : "transparent"
                    opacity: quickSettings.btQuickEnabled ? 1.0 : 0.45

                    // left half: toggle the adapter
                    MouseArea {
                        id: btLeft
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: btChevron.left
                        hoverEnabled: true
                        enabled: quickSettings.btQuickEnabled
                        onClicked: bluetoothModule.setPowered(!bluetoothModule.powered)
                    }
                    // right chevron: open the device-list sub-panel
                    MouseArea {
                        id: btChevron
                        width: 46
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.right: parent.right
                        hoverEnabled: true
                        enabled: quickSettings.btQuickEnabled
                        onClicked: quickSettings.btSubOpen()

                        // hover highlight: make the "click to enter"
                        // affordance visible on the right edge
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 6
                            radius: 6
                            color: parent.containsMouse
                                   ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, barCfgActiveBg.a)
                                   : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: "›"
                                color: "#99ffffff"
                                font.pixelSize: 22
                            }
                        }
                    }

                    Image {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        width: 22
                        height: 22
                        source: bluetoothModule.connectedCount > 0
                                ? "qrc:/icons/Bluetooth/Bluetoothcon.png"
                                : "qrc:/icons/Bluetooth/Bluetoothon.png"
                        opacity: bluetoothModule.powered ? 1.0 : 0.4
                        sourceSize: Qt.size(32, 32)
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                    }
                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 44
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text {
                            text: qsTr("蓝牙")
                            color: "#ffffff"
                            font.pixelSize: 14
                        }
                        Text {
                            text: bluetoothModule.connectedCount > 0
                                  ? qsTr("已连接 ") + bluetoothModule.connectedCount + qsTr(" 个设备")
                                  : (bluetoothModule.powered ? qsTr("已开启") : qsTr("已关闭"))
                            color: "#99ffffff"
                            font.pixelSize: 11
                        }
                    }
                }

                // ---- volume (PipeWire): mute toggle + slider ----
                Rectangle {
                    width: parent.width
                    height: 52
                    radius: 8
                    color: "transparent"
                    visible: volumeModule.available && volumeModule.sinkName !== ""

                    // mute toggle button: click the speaker icon to mute/unmute
                    MouseArea {
                        id: volMuteBtn
                        width: 36
                        height: 36
                        anchors.left: parent.left
                        anchors.leftMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        hoverEnabled: true
                        onClicked: volumeModule.toggleMute()

                        Rectangle {
                            anchors.fill: parent
                            radius: 6
                            color: parent.containsMouse
                                   ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, barCfgActiveBg.a)
                                   : "transparent"
                            Image {
                                anchors.centerIn: parent
                                width: 22
                                height: 22
                                source: root.volumeImage(volumeModule.percent, volumeModule.muted)
                                opacity: volumeModule.muted ? 0.6 : 1.0
                                sourceSize: Qt.size(32, 32)
                                fillMode: Image.PreserveAspectFit
                                smooth: true
                            }
                        }
                    }
                    Column {
                        anchors.left: parent.left
                        anchors.leftMargin: 44
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text {
                            text: qsTr("音量")
                            color: "#ffffff"
                            font.pixelSize: 14
                        }
                        Text {
                            text: volumeModule.muted
                                  ? qsTr("已静音")
                                  : volumeModule.percent + "%"
                            color: volumeModule.muted ? "#9a9a9a" : "#99ffffff"
                            font.pixelSize: 11
                        }
                    }

                    // volume slider: drag to set, keeps following the module
                    Slider {
                        id: volSlider
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        width: 130
                        height: 20
                        from: 0
                        to: 100
                        value: volumeModule.percent
                        onMoved: volumeModule.setVolume(volSlider.value)

                        background: Rectangle {
                            x: volSlider.leftPadding
                            y: volSlider.topPadding + volSlider.availableHeight / 2 - 3
                            width: volSlider.availableWidth
                            height: 6
                            radius: 3
                            color: "#4dffffff"
                            Rectangle {
                                width: volSlider.visualPosition * parent.width
                                height: parent.height
                                radius: 3
                                color: volumeModule.muted ? "#9a9a9a" : "#3a9b5f"
                            }
                        }
                        handle: Rectangle {
                            x: volSlider.leftPadding + volSlider.visualPosition * (volSlider.availableWidth - width)
                            y: volSlider.topPadding + volSlider.availableHeight / 2 - height / 2
                            width: 14
                            height: 14
                            radius: 7
                            color: volSlider.pressed ? "#ffffff" : "#e0e0e0"
                            border.color: "#66000000"
                        }
                    }
                }

            }
        }

        // ---- wifi sub-panel: scan + list networks ----
        Rectangle {
            id: wifiSubPanel
            visible: false
            width: 280
            height: 420
            onHeightChanged: if (visible) quickSettings.reAnchorQuick(wifiSubPanel)
            radius: 8
            color: Qt.rgba(barCfgMenuBg.r, barCfgMenuBg.g, barCfgMenuBg.b, barCfgMenuBg.a)
            border.color: "#55666666"
            border.width: 1
            opacity: 0
            scale: 0.95
            z: 3

            MouseArea { anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton }

            Column {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4

                // header: back + title + rescan
                RowLayout {
                    width: parent.width
                    height: 40
                    spacing: 6

                    MouseArea {
                        Layout.preferredWidth: 40
                        Layout.fillHeight: true
                        hoverEnabled: true
                        onClicked: quickSettings.quickBack()
                        Rectangle {
                            anchors.fill: parent
                            radius: 6
                            color: parent.containsMouse
                                   ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, barCfgActiveBg.a)
                                   : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: "‹"
                                color: "#ffffff"
                                font.pixelSize: 22
                            }
                        }
                    }
                    Text {
                        Layout.fillHeight: true
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("网络")
                        color: "#ffffff"
                        font.pixelSize: 14
                    }
                    Item { Layout.fillWidth: true }
                    MouseArea {
                        Layout.preferredWidth: 64
                        Layout.fillHeight: true
                        hoverEnabled: true
                        onClicked: networkModule.scanWifi()
                        Rectangle {
                            anchors.fill: parent
                            radius: 6
                            color: parent.containsMouse
                                   ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, barCfgActiveBg.a)
                                   : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("刷新")
                                color: "#ffffff"
                                font.pixelSize: 12
                            }
                        }
                    }
                }

                // network list
                ListView {
                    id: wifiList
                    width: parent.width
                    height: parent.height - 44
                    clip: true
                    model: networkModule.wifiNetworks
                    delegate: Rectangle {
                        width: wifiList.width
                        height: 40
                        radius: 6
                        color: netItemMouse.containsMouse
                               ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, barCfgActiveBg.a)
                               : "transparent"
                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.name
                                elide: Text.ElideRight
                                width: parent.width - 130
                                color: modelData.active ? "#ffffff" : "#cccccc"
                                font.pixelSize: 13
                                font.bold: modelData.active
                            }
                            Text {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.active ? qsTr("已连接") : (modelData.strength + "%")
                                color: modelData.active ? "#5fb878" : "#999999"
                                font.pixelSize: 12
                            }
                        }
                        MouseArea {
                            id: netItemMouse
                            anchors.fill: parent
                            hoverEnabled: true
                        }
                    }
                }
            }
        }

        // ---- bluetooth sub-panel: scan + list devices ----
        Rectangle {
            id: btSubPanel
            visible: false
            width: 280
            height: 420
            onHeightChanged: if (visible) quickSettings.reAnchorQuick(btSubPanel)
            radius: 8
            color: Qt.rgba(barCfgMenuBg.r, barCfgMenuBg.g, barCfgMenuBg.b, barCfgMenuBg.a)
            border.color: "#55666666"
            border.width: 1
            opacity: 0
            scale: 0.95
            z: 3

            MouseArea { anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton }

            Column {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4

                // header: back + title + scan
                RowLayout {
                    width: parent.width
                    height: 40
                    spacing: 6

                    MouseArea {
                        Layout.preferredWidth: 40
                        Layout.fillHeight: true
                        hoverEnabled: true
                        onClicked: quickSettings.quickBack()
                        Rectangle {
                            anchors.fill: parent
                            radius: 6
                            color: parent.containsMouse
                                   ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, barCfgActiveBg.a)
                                   : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: "‹"
                                color: "#ffffff"
                                font.pixelSize: 22
                            }
                        }
                    }
                    Text {
                        Layout.fillHeight: true
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("蓝牙设备")
                        color: "#ffffff"
                        font.pixelSize: 14
                    }
                    Item { Layout.fillWidth: true }
                    MouseArea {
                        Layout.preferredWidth: 64
                        Layout.fillHeight: true
                        hoverEnabled: true
                        onClicked: bluetoothModule.startDiscovery()
                        Rectangle {
                            anchors.fill: parent
                            radius: 6
                            color: parent.containsMouse
                                   ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, barCfgActiveBg.a)
                                   : "transparent"
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("扫描")
                                color: "#ffffff"
                                font.pixelSize: 12
                            }
                        }
                    }
                }

                // device list
                ListView {
                    id: btList
                    width: parent.width
                    height: parent.height - 44
                    clip: true
                    model: bluetoothModule.devices
                    delegate: Rectangle {
                        width: btList.width
                        height: 40
                        radius: 6
                        // same look as the main menu: transparent until hovered
                        color: btItemMouse.containsMouse
                               ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, barCfgActiveBg.a)
                               : "transparent"

                        // row click: connect when idle (the disconnect button
                        // below sits above this area and handles its own clicks)
                        MouseArea {
                            id: btItemMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                if (!modelData.connected)
                                    bluetoothModule.connectDevice(modelData.path)
                            }
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 8
                            spacing: 6

                            Text {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                                text: modelData.name
                                elide: Text.ElideRight
                                color: modelData.connected ? "#ffffff" : "#cccccc"
                                font.pixelSize: 13
                            }

                            // battery level, only for devices that report it
                            Row {
                                visible: modelData.battery >= 0
                                Layout.alignment: Qt.AlignVCenter
                                spacing: 3
                                Image {
                                    width: 16
                                    height: 16
                                    anchors.verticalCenter: parent.verticalCenter
                                    source: root.batteryImage(modelData.battery, false)
                                    sourceSize: Qt.size(48, 48)
                                    fillMode: Image.PreserveAspectFit
                                    smooth: true
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData.battery + "%"
                                    color: modelData.battery <= 15 ? "#ff8080" : "#999999"
                                    font.pixelSize: 11
                                    font.weight: Font.Bold
                                }
                            }

                            // connected indicator: green dot
                            Rectangle {
                                visible: modelData.connected
                                Layout.alignment: Qt.AlignVCenter
                                width: 8
                                height: 8
                                radius: 4
                                color: "#5fb878"
                            }

                            // disconnect button (connected devices only)
                            Rectangle {
                                id: disconnectBtn
                                visible: modelData.connected
                                Layout.alignment: Qt.AlignVCenter
                                width: disconnectText.implicitWidth + 16
                                height: 24
                                radius: 5
                                color: disconnectMouse.containsMouse
                                       ? Qt.rgba(barCfgActiveBg.r, barCfgActiveBg.g, barCfgActiveBg.b, barCfgActiveBg.a)
                                       : "transparent"
                                border.color: "#55ffffff"
                                Text {
                                    id: disconnectText
                                    anchors.centerIn: parent
                                    text: qsTr("断开")
                                    color: "#ffffff"
                                    font.pixelSize: 11
                                }
                                MouseArea {
                                    id: disconnectMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: bluetoothModule.disconnectDevice(modelData.path)
                                }
                            }

                            // connect/pair hint (idle devices only)
                            Text {
                                visible: !modelData.connected
                                Layout.alignment: Qt.AlignVCenter
                                text: modelData.paired ? qsTr("点击连接") : qsTr("点击配对")
                                color: "#7f9cf5"
                                font.pixelSize: 11
                            }
                        }
                    }
                }
            }
        }
    }
}
