/*
 * keycast.c - keyboard echo feed
 *
 * Listens on a local Unix domain socket and broadcasts one JSON object
 * per physical key event to every connected client (typically the showkey
 * Qt overlay).  The message carries the evdev keycode, press/release
 * state, a human-readable label translated through xkbcommon, and the
 * current depressed modifier mask:
 *
 *   {"type":"key","keycode":30,"state":1,"label":"A","mods":1}
 *
 * The socket lives at $XDG_RUNTIME_DIR/monodywm-keycast.sock with mode
 * 0600 and only accepts connections from the same uid (SO_PEERCRED), so a
 * random local process cannot join the feed by accident.
 *
 * All of this is self-contained: input.c only calls keycast_key().
 */

#define _GNU_SOURCE /* SO_PEERCRED / struct ucred */

#include "keycast.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include <wlr/util/log.h>

struct keycast_client {
	struct wl_list link;
	int fd;
	struct wl_event_source *source;

	char *out;        /* pending outgoing bytes */
	size_t out_len;   /* bytes queued */
	size_t out_off;   /* bytes already written */
	size_t out_cap;
};

struct keycast {
	struct server *server;
	int fd;                       /* listening socket, -1 when not running */
	struct wl_event_source *source; /* accept event source */
	struct wl_list clients;
};

static struct keycast keycast = { .fd = -1 };

/* ------------------------------------------------------------------ */
/* client bookkeeping (same non-blocking queue pattern as ipc.c)       */
/* ------------------------------------------------------------------ */

static void keycast_client_destroy(struct keycast_client *client) {
	wl_event_source_remove(client->source);
	close(client->fd);
	free(client->out);
	wl_list_remove(&client->link);
	free(client);
}

static bool keycast_client_flush(struct keycast_client *client) {
	while (client->out_off < client->out_len) {
		ssize_t n = write(client->fd, client->out + client->out_off,
			client->out_len - client->out_off);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return true; /* wait for the writable event */
			}
			keycast_client_destroy(client);
			return false; /* client is gone */
		}
		client->out_off += (size_t)n;
	}
	client->out_len = 0;
	client->out_off = 0;
	return true;
}

static void keycast_client_queue(struct keycast_client *client,
		const char *json) {
	size_t len = strlen(json);
	if (client->out_len + len + 1 > client->out_cap) {
		size_t cap = client->out_cap ? client->out_cap * 2 : 256;
		while (cap < client->out_len + len + 1) {
			cap *= 2;
		}
		char *buf = realloc(client->out, cap);
		if (buf == NULL) {
			return;
		}
		client->out = buf;
		client->out_cap = cap;
	}
	memcpy(client->out + client->out_len, json, len);
	client->out_len += len;
	client->out[client->out_len++] = '\n';

	if (!keycast_client_flush(client)) {
		return; /* client destroyed by a failing write */
	}
	uint32_t mask = WL_EVENT_READABLE;
	if (client->out_off < client->out_len) {
		mask |= WL_EVENT_WRITABLE;
	}
	wl_event_source_fd_update(client->source, mask);
}

static int keycast_client_handle_data(int fd, uint32_t mask, void *data) {
	struct keycast_client *client = data;
	if ((mask & WL_EVENT_WRITABLE) != 0) {
		if (!keycast_client_flush(client)) {
			return 0;
		}
		uint32_t new_mask = WL_EVENT_READABLE;
		if (client->out_off < client->out_len) {
			new_mask |= WL_EVENT_WRITABLE;
		}
		wl_event_source_fd_update(client->source, new_mask);
	}
	if ((mask & WL_EVENT_READABLE) != 0) {
		/* the client never sends us anything; a read of 0 (or <0) means
		 * it disconnected */
		char buf[256];
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n <= 0) {
			keycast_client_destroy(client);
			return 0;
		}
	}
	return 0;
}

static int keycast_handle_accept(int fd, uint32_t mask, void *data) {
	(void)data;
	if ((mask & WL_EVENT_READABLE) == 0) {
		return 0;
	}
	int cfd = accept(fd, NULL, NULL);
	if (cfd < 0) {
		return 0;
	}

	/* only the owning user may read the keystroke feed */
	struct ucred cred;
	socklen_t cred_len = sizeof(cred);
	if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0 ||
			cred.uid != geteuid()) {
		close(cfd);
		return 0;
	}

	if (fcntl(cfd, F_SETFL, O_NONBLOCK | O_CLOEXEC) < 0) {
		close(cfd);
		return 0;
	}

	struct keycast_client *client = calloc(1, sizeof(*client));
	if (client == NULL) {
		close(cfd);
		return 0;
	}
	client->fd = cfd;
	client->source = wl_event_loop_add_fd(
		wl_display_get_event_loop(keycast.server->display), cfd,
		WL_EVENT_READABLE, keycast_client_handle_data, client);
	if (client->source == NULL) {
		close(cfd);
		free(client);
		return 0;
	}
	wl_list_insert(keycast.clients.prev, &client->link);
	keycast_client_queue(client, "{\"type\":\"hello\"}");
	return 0;
}

/* ------------------------------------------------------------------ */
/* key label translation                                              */
/* ------------------------------------------------------------------ */

static void keycast_label(struct wlr_keyboard *keyboard, uint32_t evdev_code,
		char *out, size_t outlen) {
	if (keyboard == NULL || keyboard->xkb_state == NULL) {
		snprintf(out, outlen, "key%u", evdev_code);
		return;
	}

	uint32_t keycode = evdev_code + 8;
	char utf8[64] = {0};
	xkb_state_key_get_utf8(keyboard->xkb_state, keycode, utf8, sizeof(utf8));

	/* printable text (a, A, 1, ...): use it directly */
	if (utf8[0] != '\0' && utf8[0] != ' ' && utf8[0] != '\n' &&
			utf8[0] != '\r' && utf8[0] != '\t' &&
			(unsigned char)utf8[0] >= 0x20) {
		snprintf(out, outlen, "%s", utf8);
		return;
	}

	/* special keys: translate the keysym name into a friendly label */
	xkb_keysym_t sym = xkb_state_key_get_one_sym(keyboard->xkb_state, keycode);
	char name[64];
	xkb_keysym_get_name(sym, name, sizeof(name));

	static const struct { const char *from, *to; } pretty[] = {
		{"Control_L", "Ctrl"},   {"Control_R", "Ctrl"},
		{"Shift_L", "Shift"},     {"Shift_R", "Shift"},
		{"Alt_L", "Alt"},         {"Alt_R", "Alt"},
		{"Super_L", "Super"},     {"Super_R", "Super"},
		{"Caps_Lock", "Caps"},    {"Escape", "Esc"},
		{"Return", "Enter"},      {"BackSpace", "Backspace"},
		{"Tab", "Tab"},           {"ISO_Left_Tab", "Tab"},
		{"space", "Space"},       {"Delete", "Del"},
		{"Insert", "Ins"},        {"Page_Up", "PgUp"},
		{"Page_Down", "PgDn"},    {"ISO_Level3_Shift", "AltGr"},
	};
	for (size_t i = 0; i < sizeof(pretty) / sizeof(pretty[0]); i++) {
		if (strcmp(name, pretty[i].from) == 0) {
			snprintf(out, outlen, "%s", pretty[i].to);
			return;
		}
	}
	snprintf(out, outlen, "%s", name);
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

/* serialize a JSON object and broadcast it as one newline-terminated
 * message to every connected client; consumes the cJSON object */
static void keycast_broadcast(cJSON *root) {
	char *json = cJSON_PrintUnformatted(root);
	if (json != NULL) {
		struct keycast_client *client;
		wl_list_for_each(client, &keycast.clients, link) {
			keycast_client_queue(client, json);
		}
		free(json);
	}
	cJSON_Delete(root);
}

bool keycast_init(struct server *server) {
	if (keycast.fd >= 0) {
		return true; /* already running */
	}

	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	if (runtime_dir != NULL && runtime_dir[0] != '\0') {
		snprintf(path, sizeof(path), "%s/monodywm-keycast.sock", runtime_dir);
	} else {
		snprintf(path, sizeof(path), "/tmp/monodywm-keycast.sock");
	}

	unlink(path);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		return false;
	}
	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(addr.sun_path)) {
		close(fd);
		return false;
	}
	strcpy(addr.sun_path, path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return false;
	}
	chmod(path, 0600);
	if (listen(fd, 8) < 0) {
		close(fd);
		return false;
	}

	keycast.server = server;
	keycast.fd = fd;
	wl_list_init(&keycast.clients);
	keycast.source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->display), fd, WL_EVENT_READABLE,
		keycast_handle_accept, NULL);
	if (keycast.source == NULL) {
		close(fd);
		keycast.fd = -1;
		return false;
	}

	wlr_log(WLR_INFO, "keycast socket listening on %s", path);
	return true;
}

void keycast_key(struct server *server, struct wlr_keyboard *keyboard,
		uint32_t keycode, uint32_t state) {
	(void)server;
	if (keycast.fd < 0 || wl_list_empty(&keycast.clients)) {
		return;
	}

	char label[64];
	keycast_label(keyboard, keycode, label, sizeof(label));

	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return;
	}
	cJSON_AddStringToObject(root, "type", "key");
	cJSON_AddNumberToObject(root, "keycode", keycode);
	cJSON_AddNumberToObject(root, "state", state);
	cJSON_AddStringToObject(root, "label", label);
	if (keyboard != NULL) {
		cJSON_AddNumberToObject(root, "mods", keyboard->modifiers.depressed);
	} else {
		cJSON_AddNumberToObject(root, "mods", 0);
	}
	keycast_broadcast(root);
}

void keycast_button(struct server *server, uint32_t button, uint32_t state) {
	(void)server;
	if (keycast.fd < 0 || wl_list_empty(&keycast.clients)) {
		return;
	}

	const char *label;
	switch (button) {
	case BTN_LEFT:   label = "LMB"; break;
	case BTN_RIGHT:  label = "RMB"; break;
	case BTN_MIDDLE: label = "MMB"; break;
	default:         label = "Btn";  break;
	}

	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return;
	}
	cJSON_AddStringToObject(root, "type", "button");
	cJSON_AddNumberToObject(root, "button", button);
	cJSON_AddNumberToObject(root, "state", state);
	cJSON_AddStringToObject(root, "label", label);
	keycast_broadcast(root);
}

void keycast_scroll(struct server *server, uint32_t orientation,
		int32_t delta_discrete) {
	(void)server;
	if (keycast.fd < 0 || wl_list_empty(&keycast.clients) ||
			delta_discrete == 0) {
		return;
	}

	const char *label;
	if (orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		label = delta_discrete < 0 ? "Scroll ↑" : "Scroll ↓";
	} else if (orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
		label = delta_discrete < 0 ? "Scroll ←" : "Scroll →";
	} else {
		label = "Scroll";
	}

	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return;
	}
	cJSON_AddStringToObject(root, "type", "scroll");
	cJSON_AddNumberToObject(root, "orientation", orientation);
	cJSON_AddNumberToObject(root, "delta", delta_discrete);
	cJSON_AddStringToObject(root, "label", label);
	keycast_broadcast(root);
}

void keycast_finish(struct server *server) {
	(void)server;
	if (keycast.fd < 0) {
		return;
	}
	struct keycast_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &keycast.clients, link) {
		keycast_client_destroy(client);
	}
	if (keycast.source != NULL) {
		wl_event_source_remove(keycast.source);
		keycast.source = NULL;
	}
	close(keycast.fd);
	keycast.fd = -1;
	keycast.server = NULL;
}
