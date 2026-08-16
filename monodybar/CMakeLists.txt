cmake_minimum_required(VERSION 3.16)
project(monodybar VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml Network)

qt_standard_project_setup()

find_package(LayerShellQt REQUIRED)

qt_add_executable(monodybar
    src/main.cpp
    src/BarConfig.h
    src/BarController.cpp
    src/BarController.h
    src/DesktopApps.cpp
    src/DesktopApps.h
    src/IconFinder.cpp
    src/IconFinder.h
    src/IconProvider.cpp
    src/IconProvider.h
)

target_include_directories(monodybar PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

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
    LayerShellQt::Interface
)

qt_add_resources(monodybar "qml"
    PREFIX "/"
    BASE ${CMAKE_CURRENT_SOURCE_DIR}
    FILES qml/main.qml
          win.png
)

install(TARGETS monodybar RUNTIME DESTINATION bin)
