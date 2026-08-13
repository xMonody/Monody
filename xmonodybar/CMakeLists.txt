cmake_minimum_required(VERSION 3.16)
project(qt6-bar VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml Network)

qt_standard_project_setup()

# layer-shell-qt ships only a runtime library here (no -dev package), so link
# the versioned .so directly. Public headers live in 3rdparty/LayerShellQt.
find_library(LAYER_SHELL_QT_LIB
    NAMES libLayerShellQtInterface.so.6 LayerShellQtInterface
    HINTS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE} /usr/lib64
)
if(NOT LAYER_SHELL_QT_LIB)
    message(FATAL_ERROR "layer-shell-qt runtime library (libLayerShellQtInterface) not found")
endif()

qt_add_executable(qt6-bar
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

target_include_directories(qt6-bar PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty
)

# Debian/Ubuntu Qt6 keeps QT_SKIP_AUTO_QML_PLUGIN_INCLUSION=ON, so the QML
# plugin targets (qtquick2plugin, qmlplugin, ...) never exist in scope and the
# automatic plugin import in qt6_finalize_target warns about each one. The
# plugins are loaded from the Qt plugin directory at runtime anyway (shared
# build), so skip the import scan to silence those warnings.
set_target_properties(qt6-bar PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)

target_link_libraries(qt6-bar PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Qml
    Qt6::Network
    ${LAYER_SHELL_QT_LIB}
)

qt_add_resources(qt6-bar "qml"
    PREFIX "/"
    BASE ${CMAKE_CURRENT_SOURCE_DIR}
    FILES qml/main.qml
          win.png
)

install(TARGETS qt6-bar RUNTIME DESTINATION bin)
