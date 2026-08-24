cmake_minimum_required(VERSION 3.16)
project(monodybar VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml Network DBus)

qt_standard_project_setup()

find_package(LayerShellQt REQUIRED)

qt_add_executable(monodybar
    src/main.cpp
    src/BarConfig.h
    src/BarController.cpp
    src/BarController.h
    src/DesktopApps.cpp
    src/DesktopApps.h
    src/IconIndex.cpp
    src/IconIndex.h
    src/IconProvider.cpp
    src/IconProvider.h
    src/NotificationDaemon.cpp
    src/NotificationDaemon.h
    src/TrayIconProvider.cpp
    src/TrayIconProvider.h
    src/TrayItem.cpp
    src/TrayItem.h
    src/TrayModel.cpp
    src/TrayModel.h
    src/TrayWatcher.cpp
    src/TrayWatcher.h
    module/ModuleBase.h
    module/BatteryModule.h
    module/NetworkModule.h
    module/BluetoothModule.h
    module/VolumeModule.h
    module/PowerModule.h
)

target_include_directories(monodybar PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/module
)

# PipeWire native API (libpipewire-0.3) for the volume module.  The distro
# does not ship libpipewire-0.3-dev, so the headers and the link stub are
# unpacked into a local sysroot (see ~/.local/pwdev).  Once the dev package
# is installed system-wide, drop the sysroot indirection below.
set(PWDEV_SYSROOT "$ENV{HOME}/.local/pwdev/sysroot")
if(EXISTS "${PWDEV_SYSROOT}/usr/include/pipewire-0.3")
    target_include_directories(monodybar PRIVATE
        ${PWDEV_SYSROOT}/usr/include/pipewire-0.3
        ${PWDEV_SYSROOT}/usr/include/spa-0.2
    )
    target_link_directories(monodybar PRIVATE ${PWDEV_SYSROOT}/usr/lib/x86_64-linux-gnu)
    target_link_libraries(monodybar PRIVATE pipewire-0.3)
else()
    message(FATAL_ERROR
        "PipeWire dev headers not found. Install libpipewire-0.3-dev / libspa-0.2-dev "
        "or unpack them into ${PWDEV_SYSROOT} (VolumeModule.h needs them).")
endif()

# Debian/Ubuntu Qt6 keeps QT_SKIP_AUTO_QML_PLUGIN_INCLUSION=ON, so the QML
# plugin targets (qtquick2plugin, qmlplugin, ...) never exist in scope and the
# automatic plugin import in qt6_finalize_target warns about each one. The
# plugins are loaded from the Qt plugin directory at runtime anyway (shared
# build), so skip the import scan to silence those warnings.
set_target_properties(monodybar PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)

target_link_libraries(monodybar PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Qml
    Qt6::Network
    Qt6::DBus
    LayerShellQt::Interface
)

qt_add_resources(monodybar "qml"
    PREFIX "/"
    BASE ${CMAKE_CURRENT_SOURCE_DIR}
    FILES qml/main.qml
          qml/LiquidGlass.qml
          qml/ColorUtils.qml
          qml/WinStart.qml
          qml/AppIcons.qml
          qml/Tray.qml
          qml/StatusBar.qml
          win.png
          protocol/org.freedesktop.Notifications.xml
)

# Status-module icons (Battery / Bluetooth / network), served to QML as
# qrc:/icons/... by the D-Bus modules in module/.
qt_add_resources(monodybar "icons"
    PREFIX "/icons"
    BASE ${CMAKE_CURRENT_SOURCE_DIR}/icons
    FILES
        icons/Battery/Battery0-15.png
        icons/Battery/Battery15-30.png
        icons/Battery/Battery30-45.png
        icons/Battery/Battery45-60.png
        icons/Battery/Battery60-75.png
        icons/Battery/Battery75-90.png
        icons/Battery/Battery90-100.png
        icons/Battery/BatteryChargin0-15.png
        icons/Battery/BatteryCharging15-30.png
        icons/Battery/BatteryCharging30-45.png
        icons/Battery/BatteryCharging45-60.png
        icons/Battery/BatteryCharging60-75.png
        icons/Battery/BatteryCharging75-90.png
        icons/Battery/BatteryCharging90-100.png
        icons/Bluetooth/Bluetoothcon.png
        icons/Bluetooth/Bluetoothon.png
        icons/network/DisconnectWifi.png
        icons/network/SconnectWifi.png
        icons/network/WiredConnection.png
        icons/Volume/Volume0.png
        icons/Volume/Volume1-33.png
        icons/Volume/Volume33-66.png
        icons/Volume/Volume66-100.png
        icons/Power/Shutdown.png
        icons/Power/Restart.png
        icons/Power/Sleep.png
        icons/Power/Lock.png
)

install(TARGETS monodybar RUNTIME DESTINATION bin)
