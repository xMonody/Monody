project('xmonodywm', 'c',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>=0.63.0',
	default_options: [
		'c_std=c11',
		'warning_level=2',
	])

wlroots = dependency('wlroots-0.19')
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
cjson = dependency('libcjson')

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

# The wlroots .pc file doesn't install the generated protocol headers
# (wlr-layer-shell-unstable-v1-protocol.h etc.). Point at the wlroots build
# dir that contains them. Override with:
#   meson setup build -Dwlroots_protocol_dir=/path/to/wlroots/build/protocol
wlroots_protocol_dir = get_option('wlroots_protocol_dir')
wlroots_protocol_inc = []
if wlroots_protocol_dir != ''
	wlroots_protocol_inc = include_directories(wlroots_protocol_dir)
endif

executable(
	'xmonodywm',
	files('main.c'),
	include_directories: wlroots_protocol_inc,
	dependencies: [wlroots, wayland_server, xkbcommon, cjson],
	install: true,
)
