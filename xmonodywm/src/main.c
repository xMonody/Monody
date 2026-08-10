/*
 * xmonodywm - a minimal floating Wayland compositor built on wlroots 0.19
 *
 * Entry point: creates the display, backend, renderer and scene, creates
 * every protocol global (through wlroots, which implements them
 * server-side; the XML descriptions live in Protocol/ and are turned into
 * .h/.c by wayland-scanner at build time), registers the module listeners
 * and runs the event loop.
 *
 * Modules:
 *   ipc.c      - status-bar socket (JSON events)
 *   scene.c    - scene-graph tagging / hit-testing
 *   toplevel.c - xdg-shell windows, window state, decorations
 *   border.c   - rounded server-side border for undecorated windows
 *   blur.c     - GLSL gaussian background blur for transparent windows
 *   layer.c    - wlr-layer-shell surfaces + work area
 *   output.c   - monitors + wlr-output-management
 *   input.c    - seat, keyboard, shortcuts
 *   pointer.c  - cursor interaction (move / resize / gestures)
 */

#include "server.h"

#include "ipc.h"

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/log.h>

/* run a command in a detached child process:
 *   - setsid(): new session, no controlling terminal, immune to terminal
 *     signals (Ctrl+C, SIGHUP when the tty closes), keeps running even if
 *     the compositor's terminal goes away
 *   - stdin/stdout/stderr -> /dev/null: no output polluting the compositor's
 *     tty, and the process can never block on a terminal read/write
 *   - _exit(127) if exec fails: never fall through into the compositor */
void spawn_command(const char *cmd) {
	pid_t pid = fork();
	if (pid != 0) {
		return;
	}
	setsid();
	int devnull = open("/dev/null", O_RDWR);
	if (devnull >= 0) {
		dup2(devnull, STDIN_FILENO);
		dup2(devnull, STDOUT_FILENO);
		dup2(devnull, STDERR_FILENO);
		if (devnull > STDERR_FILENO) {
			close(devnull);
		}
	}
	execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
	_exit(127);
}

/* reap spawned children (startup commands, the -s command, the terminal
 * shortcut) so exited helpers don't pile up as zombies */
static void reap_children(int sig) {
	(void)sig;
	while (waitpid(-1, NULL, WNOHANG) > 0) {
	}
}

/* launch the user's startup commands, one per line, from
 * $XDG_CONFIG_HOME/mywm/run (or ~/.config/mywm/run): daemons, wallpapers,
 * output configuration, input methods, ...  Blank lines and lines starting
 * with '#' are ignored.  Each line is run through /bin/sh -c, so ~, $VARS,
 * quotes and shell syntax all work.  Called only after the backend is up
 * and WAYLAND_DISPLAY is set, so the commands can talk to the compositor. */
static void run_startup_file(void) {
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char path[4096];
	if (xdg != NULL && xdg[0] != '\0') {
		snprintf(path, sizeof(path), "%s/mywm/run", xdg);
	} else if (home != NULL && home[0] != '\0') {
		snprintf(path, sizeof(path), "%s/.config/mywm/run", home);
	} else {
		return;
	}

	FILE *f = fopen(path, "r");
	if (f == NULL) {
		wlr_log(WLR_DEBUG, "startup: no %s, nothing to run", path);
		return;
	}

	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	while ((len = getline(&line, &cap, f)) != -1) {
		/* strip the trailing newline / carriage return */
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = '\0';
		}
		/* skip leading whitespace, blank lines and comments */
		char *cmd = line;
		while (*cmd == ' ' || *cmd == '\t') {
			cmd++;
		}
		if (*cmd == '\0' || *cmd == '#') {
			continue;
		}
		/* drop a trailing shell comment (a '#' outside quotes/escapes) and
		 * any trailing '&' / ';' / whitespace, so every line can safely be
		 * backgrounded below: an explicit '&' already present would turn
		 * into a 'cmd & &' syntax error, and a trailing comment would
		 * swallow the appended '&' */
		char *comment = NULL;
		char quote = '\0';
		for (char *p = cmd; *p != '\0'; p++) {
			if (quote != '\0') {
				if (*p == quote) {
					quote = '\0';
				} else if (*p == '\\' && quote == '"') {
					p++; /* escaped char inside double quotes */
				}
			} else if (*p == '\'' || *p == '"') {
				quote = *p;
			} else if (*p == '\\') {
				p++; /* escaped char (e.g. \#): skip both */
			} else if (*p == '#') {
				comment = p;
				break;
			}
		}
		if (comment != NULL) {
			*comment = '\0';
		}
		char *end = cmd + strlen(cmd);
		while (end > cmd && (end[-1] == ' ' || end[-1] == '\t' ||
				end[-1] == '&' || end[-1] == ';')) {
			*--end = '\0';
		}
		if (*cmd == '\0') {
			continue; /* e.g. the line was only a comment */
		}
		/* launch every line in the background */
		size_t cmdlen = strlen(cmd);
		char *bg = malloc(cmdlen + 3);
		if (bg == NULL) {
			continue;
		}
		memcpy(bg, cmd, cmdlen);
		bg[cmdlen] = ' ';
		bg[cmdlen + 1] = '&';
		bg[cmdlen + 2] = '\0';
		wlr_log(WLR_INFO, "startup: %s", bg);
		spawn_command(bg);
		free(bg);
	}
	free(line);
	fclose(f);
}

int main(int argc, char *argv[]) {
	/* WLR_DEBUG=1 switches to debug logging (the bundled tests verify cursor
	 * decisions through the "cursor: ..." WLR_DEBUG lines) */
	const char *dbg = getenv("WLR_DEBUG");
	wlr_log_init(dbg != NULL && dbg[0] != '\0' ? WLR_DEBUG : WLR_INFO, NULL);
	/* never leave spawned helpers as zombies */
	struct sigaction chld_sa = {0};
	chld_sa.sa_handler = reap_children;
	sigemptyset(&chld_sa.sa_mask);
	chld_sa.sa_flags = SA_RESTART;
	sigaction(SIGCHLD, &chld_sa, NULL);

	char *startup_cmd = NULL;
	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			fprintf(stderr, "usage: %s [-s startup-command]\n", argv[0]);
			return EXIT_FAILURE;
		}
	}

	struct server server = {0};

	server.display = wl_display_create();
	if (server.display == NULL) {
		return EXIT_FAILURE;
	}
	server.backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server.display), NULL);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create backend");
		return EXIT_FAILURE;
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create renderer");
		return EXIT_FAILURE;
	}
	wlr_renderer_init_wl_display(server.renderer, server.display);
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create allocator");
		return EXIT_FAILURE;
	}
	server.scene = wlr_scene_create();
	if (server.scene == NULL) {
		wlr_log(WLR_ERROR, "failed to create scene");
		return EXIT_FAILURE;
	}
	server.output_layout = wlr_output_layout_create(server.display);
	server.output_manager = wlr_output_manager_v1_create(server.display);
	server.foreign_toplevel_manager =
		wlr_foreign_toplevel_manager_v1_create(server.display);

	server.seat = wlr_seat_create(server.display, "seat0");
	server.cursor = wlr_cursor_create();
	if (server.cursor == NULL) {
		wlr_log(WLR_ERROR, "failed to create cursor");
		return EXIT_FAILURE;
	}
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.xcursor_manager = wlr_xcursor_manager_create(NULL, 24);
	if (server.xcursor_manager == NULL) {
		wlr_log(WLR_ERROR, "failed to create xcursor manager");
		return EXIT_FAILURE;
	}

	for (int i = 0; i < LAYER_COUNT; i++) {
		server.layers[i] = wlr_scene_tree_create(&server.scene->tree);
		if (server.layers[i] == NULL) {
			wlr_log(WLR_ERROR, "failed to create scene tree");
			return EXIT_FAILURE;
		}
	}
	wl_list_init(&server.toplevels);
	wl_list_init(&server.layer_surfaces);
	wl_list_init(&server.imes);
	wl_list_init(&server.text_inputs);
	wl_list_init(&server.keyboards);

	/* ---- core & stable protocols ---- */
	wlr_compositor_create(server.display, 6, server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_shm_create_with_renderer(server.display, 1, server.renderer);
	wlr_linux_dmabuf_v1_create_with_renderer(server.display, 5,
		server.renderer);
	wlr_data_device_manager_create(server.display);
	struct wlr_xdg_shell *xdg_shell =
		wlr_xdg_shell_create(server.display, 6);
	wlr_viewporter_create(server.display);
	wlr_presentation_create(server.display, server.backend, 2);

	/* ---- wlroots protocols ---- */
	struct wlr_layer_shell_v1 *layer_shell =
		wlr_layer_shell_v1_create(server.display, 5);
	struct wlr_xdg_decoration_manager_v1 *decoration_manager =
		wlr_xdg_decoration_manager_v1_create(server.display);
	struct wlr_virtual_pointer_manager_v1 *virtual_pointer_manager =
		wlr_virtual_pointer_manager_v1_create(server.display);

	/* cursor-shape-v1: clients pick a shape, the compositor renders it from
	 * its own xcursor theme at the output's (fractional) scale, so the
	 * cursor size always matches - no client-side guessing */
	server.cursor_shape_manager =
		wlr_cursor_shape_manager_v1_create(server.display, 1);

	/* ---- input method (fcitx5 / ibus) ---- */
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_manager =
		wlr_virtual_keyboard_manager_v1_create(server.display);
	struct wlr_input_method_manager_v2 *input_method_manager =
		wlr_input_method_manager_v2_create(server.display);
	struct wlr_text_input_manager_v3 *text_input_manager =
		wlr_text_input_manager_v3_create(server.display);

	/* ---- listeners ---- */
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.new_virtual_pointer.notify = server_new_virtual_pointer;
	wl_signal_add(&virtual_pointer_manager->events.new_virtual_pointer, &server.new_virtual_pointer);
	server.new_virtual_keyboard.notify = server_new_virtual_keyboard;
	wl_signal_add(&virtual_keyboard_manager->events.new_virtual_keyboard, &server.new_virtual_keyboard);
	server.layout_change.notify = server_layout_change;
	wl_signal_add(&server.output_layout->events.change, &server.layout_change);
	server.output_manager_apply.notify = output_manager_apply;
	wl_signal_add(&server.output_manager->events.apply, &server.output_manager_apply);
	server.output_manager_test.notify = output_manager_test;
	wl_signal_add(&server.output_manager->events.test, &server.output_manager_test);
	server.new_xdg_toplevel.notify = server_new_toplevel;
	wl_signal_add(&xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&layer_shell->events.new_surface, &server.new_layer_surface);
	server.new_decoration.notify = server_new_decoration;
	wl_signal_add(&decoration_manager->events.new_toplevel_decoration, &server.new_decoration);
	server.new_ime.notify = ime_new_input_method;
	wl_signal_add(&input_method_manager->events.input_method, &server.new_ime);
	server.new_text_input.notify = ime_new_text_input;
	wl_signal_add(&text_input_manager->events.text_input, &server.new_text_input);

	server.cursor_motion.notify = cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
	server.cursor_button.notify = cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	server.seat_request_set_cursor.notify = seat_request_set_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor, &server.seat_request_set_cursor);
	server.cursor_shape_set_shape.notify = seat_request_set_shape;
	wl_signal_add(&server.cursor_shape_manager->events.request_set_shape, &server.cursor_shape_set_shape);
	server.seat_request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection, &server.seat_request_set_selection);
	server.seat_request_set_primary_selection.notify = seat_request_set_primary_selection;
	wl_signal_add(&server.seat->events.request_set_primary_selection, &server.seat_request_set_primary_selection);
	server.seat_request_start_drag.notify = seat_request_start_drag;
	wl_signal_add(&server.seat->events.request_start_drag, &server.seat_request_start_drag);
	server.seat_start_drag.notify = seat_start_drag;
	wl_signal_add(&server.seat->events.start_drag, &server.seat_start_drag);

	wlr_seat_set_capabilities(server.seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

	const char *socket = wl_display_add_socket_auto(server.display);
	if (socket == NULL) {
		wlr_log(WLR_ERROR, "failed to add socket");
		return EXIT_FAILURE;
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		return EXIT_FAILURE;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	/* the Wayland socket is live: the WM is up, now start the user's
	 * daemons from ~/.config/mywm/run (plus any -s command) */
	run_startup_file();
	if (startup_cmd != NULL) {
		spawn_command(startup_cmd);
	}

	/* IPC socket for status bars (JSON over a Unix domain socket) */
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	char ipc_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	if (runtime_dir != NULL && runtime_dir[0] != '\0') {
		snprintf(ipc_path, sizeof(ipc_path), "%s/xmonodywm.sock",
			runtime_dir);
	} else {
		snprintf(ipc_path, sizeof(ipc_path), "/tmp/xmonodywm.sock");
	}
	server.ipc_fd = -1;
	if (!ipc_server_init(&server, ipc_path)) {
		wlr_log(WLR_ERROR, "failed to start IPC socket at %s", ipc_path);
	}

	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
		socket);
	wl_display_run(server.display);

	/* wlroots asserts that the objects it owns (output layout, output
	 * manager, seat, protocol managers, ...) are destroyed without leftover
	 * listeners, so detach every compositor listener before teardown. The
	 * per-surface listeners (toplevels, layer surfaces, ime, cursors) are
	 * removed by their own destroy handlers during destroy_clients(). */
	wl_list_remove(&server.new_output.link);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.new_virtual_pointer.link);
	wl_list_remove(&server.new_virtual_keyboard.link);
	wl_list_remove(&server.layout_change.link);
	wl_list_remove(&server.output_manager_apply.link);
	wl_list_remove(&server.output_manager_test.link);
	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_layer_surface.link);
	wl_list_remove(&server.new_decoration.link);
	wl_list_remove(&server.new_ime.link);
	wl_list_remove(&server.new_text_input.link);
	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);
	wl_list_remove(&server.seat_request_set_cursor.link);
	wl_list_remove(&server.cursor_shape_set_shape.link);
	wl_list_remove(&server.seat_request_set_selection.link);
	wl_list_remove(&server.seat_request_set_primary_selection.link);
	wl_list_remove(&server.seat_request_start_drag.link);
	wl_list_remove(&server.seat_start_drag.link);

	ipc_server_destroy(&server);
	blur_finish(&server);
	wl_display_destroy_clients(server.display);
	wl_display_destroy(server.display);
	return EXIT_SUCCESS;
}
