cmake_minimum_required(VERSION 3.16)
project(qt6-bar VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Network)

qt_standard_project_setup()

# layer-shell-qt ships only a runtime library here (no -dev package), so link the
# versioned .so directly. Public headers live in 3rdparty/LayerShellQt.
set(LAYER_SHELL_QT_LIB "/usr/lib/x86_64-linux-gnu/libLayerShellQtInterface.so.6")

qt_add_executable(qt6-bar
    src/main.cpp
    src/BarController.cpp
    src/BarController.h
    src/DesktopApps.cpp
    src/DesktopApps.h
    src/IconFinder.cpp
    src/IconFinder.h
)

target_include_directories(qt6-bar PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty
)

target_link_libraries(qt6-bar PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
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
