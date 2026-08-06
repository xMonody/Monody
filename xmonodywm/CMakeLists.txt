cmake_minimum_required(VERSION 3.16)
project(xmonodywm VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# wlroots exposes unstable headers through this define
add_compile_definitions(WLR_USE_UNSTABLE)

find_package(PkgConfig REQUIRED)

pkg_check_modules(WLROOTS REQUIRED IMPORTED_TARGET wlroots-0.19)
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLES2 REQUIRED IMPORTED_TARGET glesv2)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

# ---------------------------------------------------------------------------
# Protocol code generation
#
# Every protocol this project speaks lives as an XML description in
# Protocol/.  wayland-scanner turns each of them into three artifacts in the
# build tree:
#
#   <name>-protocol.h          (server-side header, wlroots naming:
#                                wlroots' installed headers include these)
#   <name>-client-protocol.h  (client-side header)
#   <name>-protocol.c         (marshalling code, generated like the user
#                              asked: cmake generates .h *and* .c from xml)
#
# The compositor itself does not link any of this code: as a wlroots
# compositor, wlroots implements the protocol globals server-side and
# registers them with the display.  It only needs the generated server
# headers on the include path, because the installed wlroots headers
# (e.g. wlr_layer_shell_v1.h) #include them.  The generated client code
# is what the bundled test clients link against, which makes the tests
# self-contained (no dependency on a wlroots build tree anymore).
# ---------------------------------------------------------------------------

set(PROTOCOLS
	wayland
	xdg-shell
	xdg-decoration-unstable-v1
	viewporter
	presentation-time
	linux-dmabuf-v1
	primary-selection-unstable-v1
	wlr-layer-shell-unstable-v1
	wlr-output-management-unstable-v1
	wlr-foreign-toplevel-management-unstable-v1
	wlr-virtual-pointer-unstable-v1
	virtual-keyboard-unstable-v1
	input-method-unstable-v2
	text-input-unstable-v3
)

set(PROTOCOL_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocol)
file(MAKE_DIRECTORY ${PROTOCOL_GEN_DIR})

# generates the three artifacts for one protocol xml file; appends their
# paths to the given output variables
function(wayland_generate name)
	set(xml ${CMAKE_CURRENT_SOURCE_DIR}/Protocol/${name}.xml)
	set(prefix ${PROTOCOL_GEN_DIR}/${name})

	add_custom_command(
		OUTPUT ${prefix}-protocol.h
		COMMAND ${WAYLAND_SCANNER} server-header ${xml} ${prefix}-protocol.h
		DEPENDS ${xml}
	)
	add_custom_command(
		OUTPUT ${prefix}-client-protocol.h
		COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${prefix}-client-protocol.h
		DEPENDS ${xml}
	)
	add_custom_command(
		OUTPUT ${prefix}-protocol.c
		COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${prefix}-protocol.c
		DEPENDS ${xml}
	)

	# collect them for the caller
	set(${name}_SERVER_HEADER ${prefix}-protocol.h PARENT_SCOPE)
	set(${name}_CLIENT_HEADER ${prefix}-client-protocol.h PARENT_SCOPE)
	set(${name}_CODE ${prefix}-protocol.c PARENT_SCOPE)
	set(PROTOCOL_GEN_FILES ${PROTOCOL_GEN_FILES} ${prefix}-protocol.h
		${prefix}-client-protocol.h ${prefix}-protocol.c PARENT_SCOPE)
endfunction()

set(PROTOCOL_GEN_FILES)
foreach(p ${PROTOCOLS})
	wayland_generate(${p})
endforeach()


# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

set(COMPOSITOR_SOURCES
	src/main.c
	src/ipc.c
	src/scene.c
	src/ime.c
	src/toplevel.c
	src/border.c
	src/blur.c
	src/layer.c
	src/output.c
	src/input.c
	src/pointer.c
)

add_library(protocol_stubs STATIC
	${xdg-shell_CODE}
	${xdg-decoration-unstable-v1_CODE}
	${wlr-virtual-pointer-unstable-v1_CODE}
	${virtual-keyboard-unstable-v1_CODE}
	${input-method-unstable-v2_CODE}
	${text-input-unstable-v3_CODE}
)
target_include_directories(protocol_stubs PUBLIC ${PROTOCOL_GEN_DIR})
target_link_libraries(protocol_stubs PUBLIC PkgConfig::WAYLAND_CLIENT)

add_executable(xmonodywm ${COMPOSITOR_SOURCES})
target_include_directories(xmonodywm PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${PROTOCOL_GEN_DIR} # wlroots' installed headers #include generated ones
)
target_link_libraries(xmonodywm PRIVATE
	PkgConfig::WLROOTS
	PkgConfig::WAYLAND_SERVER
	PkgConfig::XKBCOMMON
	PkgConfig::CJSON
	PkgConfig::EGL
	PkgConfig::GLES2
	m
)
install(TARGETS xmonodywm RUNTIME DESTINATION bin)

# bundled test clients (exercise the compositor; need the generated
# client-side protocol code)
add_executable(test-client test-client.c)
target_include_directories(test-client PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-client PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT)

add_executable(test-interaction test-interaction.c)
target_include_directories(test-interaction PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-interaction PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT)

# input-method relay test: app (text-input-v3) <-> IM (input-method-v2)
add_executable(test-ime-relay test-ime-relay.c)
target_include_directories(test-ime-relay PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-ime-relay PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON)

# fcitx5 end-to-end: app + virtual keyboard driver (needs a real fcitx5)
add_executable(test-ime-app test-ime-app.c)
target_include_directories(test-ime-app PRIVATE ${PROTOCOL_GEN_DIR})
target_link_libraries(test-ime-app PRIVATE protocol_stubs PkgConfig::WAYLAND_CLIENT PkgConfig::XKBCOMMON)

# force every generated artifact to exist before the compilers run; the
# executables only reference some of them directly, so a plain dependency
# on the target is enough for the rest
add_custom_target(protocols DEPENDS ${PROTOCOL_GEN_FILES})
add_dependencies(xmonodywm protocols)
add_dependencies(protocol_stubs protocols)
add_dependencies(test-client protocols)
add_dependencies(test-interaction protocols)
add_dependencies(test-ime-relay protocols)
add_dependencies(test-ime-app protocols)
