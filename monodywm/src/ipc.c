/*
 * ipc.c - status-bar communication over a Unix domain socket
 *
 * The compositor listens on $XDG_RUNTIME_DIR/xmonodywm.sock (fallback
 * /tmp/xmonodywm.sock).  Status bars connect and receive newline-delimited
 * JSON messages; each window is identified by a stable id.
 *
 * Events broadcast to every client: window_added, window_removed,
 * window_focus (id 0 = nothing focused), window_full, window_list.
 * Client requests (one JSON object per line): list_windows,
 * focus_window {"id": N}, close_window {"id": N},
 * maximize_window {"id": N} (toggles), minimize_window {"id": N}.
 */

#include "ipc.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <wlr/util/log.h>

/* one connected status bar client; JSON messages are newline-delimited */
struct ipc_client {
	struct wl_list link; /* server.ipc_clients */
	struct server *server;
	int fd;
	struct wl_event_source *source;

	char *out;        /* pending outgoing bytes */
	size_t out_len;   /* bytes queued */
	size_t out_off;   /* bytes already written */
	size_t out_cap;

	char in[4096];    /* partial incoming line */
	size_t in_len;
};

static void ipc_send_window_list(struct server *server,
		struct ipc_client *target);

static void ipc_client_destroy(struct ipc_client *client) {
	wl_event_source_remove(client->source);
	close(client->fd);
	free(client->out);
	wl_list_remove(&client->link);
	free(client);
}

/* try to write all queued output; on success the buffer is emptied.
 * Returns false when the client was destroyed by a failing write (the
 * caller must not touch the client afterwards). */
static bool ipc_client_flush(struct ipc_client *client) {
	while (client->out_off < client->out_len) {
		ssize_t n = write(client->fd, client->out + client->out_off,
			client->out_len - client->out_off);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return true; /* wait for the writable event */
			}
			ipc_client_destroy(client);
			return false; /* the client is gone */
		}
		client->out_off += n;
	}
	client->out_len = 0;
	client->out_off = 0;
	return true;
}

static void ipc_client_queue(struct ipc_client *client, const char *json) {
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
	/* the flush may destroy the client (failing write to a disconnected
	 * bar): never touch it again in that case */
	if (!ipc_client_flush(client)) {
		return;
	}
	/* keep the writable event enabled while there is pending output */
	uint32_t mask = WL_EVENT_READABLE;
	if (client->out_off < client->out_len) {
		mask |= WL_EVENT_WRITABLE;
	}
	wl_event_source_fd_update(client->source, mask);
}

/* find a live toplevel by its IPC id */
static struct toplevel *toplevel_by_id(struct server *server, int id) {
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		if (tl->id == id) {
			return tl;
		}
	}
	return NULL;
}

/* handle one complete JSON message received from a client */
static void ipc_handle_line(struct server *server, struct ipc_client *client,
		const char *line) {
	cJSON *root = cJSON_Parse(line);
	if (root == NULL) {
		return;
	}
	cJSON *action = cJSON_GetObjectItem(root, "action");
	if (action != NULL && cJSON_IsString(action)) {
		if (strcmp(action->valuestring, "list_windows") == 0) {
			ipc_send_window_list(server, client);
		} else if (strcmp(action->valuestring, "focus_window") == 0) {
			/* a taskbar clicked a window icon: switch focus to it */
			cJSON *id = cJSON_GetObjectItem(root, "id");
			if (id != NULL && cJSON_IsNumber(id)) {
				struct toplevel *tl =
					toplevel_by_id(server, (int)id->valuedouble);
				if (tl != NULL) {
					focus_window(server, tl);
				}
			}
		} else if (strcmp(action->valuestring, "close_window") == 0) {
			cJSON *id = cJSON_GetObjectItem(root, "id");
			if (id != NULL && cJSON_IsNumber(id)) {
				struct toplevel *tl =
					toplevel_by_id(server, (int)id->valuedouble);
				if (tl != NULL) {
					close_toplevel(tl);
				}
			}
		} else if (strcmp(action->valuestring, "maximize_window") == 0) {
			/* toggle: maximize when restored, restore when maximized */
			cJSON *id = cJSON_GetObjectItem(root, "id");
			if (id != NULL && cJSON_IsNumber(id)) {
				struct toplevel *tl =
					toplevel_by_id(server, (int)id->valuedouble);
				if (tl != NULL) {
					set_maximized(server, tl,
						!tl->xdg_toplevel->current.maximized);
				}
			}
		} else if (strcmp(action->valuestring, "minimize_window") == 0) {
			cJSON *id = cJSON_GetObjectItem(root, "id");
			if (id != NULL && cJSON_IsNumber(id)) {
				struct toplevel *tl =
					toplevel_by_id(server, (int)id->valuedouble);
				if (tl != NULL) {
					set_minimized(server, tl, true);
				}
			}
		}
	}
	cJSON_Delete(root);
}

static void ipc_client_handle_input(struct server *server,
		struct ipc_client *client, const char *data, size_t len) {
	size_t i = 0;
	while (i < len) {
		while (i < len && data[i] != '\n' &&
				client->in_len < sizeof(client->in) - 1) {
			client->in[client->in_len++] = data[i++];
		}
		if (i < len && data[i] == '\n') {
			client->in[client->in_len] = '\0';
			if (client->in_len > 0) {
				ipc_handle_line(server, client, client->in);
			}
			client->in_len = 0;
			i++;
		}
	}
}

static int ipc_client_handle_data(int fd, uint32_t mask, void *data) {
	struct ipc_client *client = data;
	struct server *server = client->server;
	if ((mask & WL_EVENT_WRITABLE) != 0) {
		if (!ipc_client_flush(client)) {
			return 0; /* the client was destroyed by the failing write */
		}
		uint32_t new_mask = WL_EVENT_READABLE;
		if (client->out_off < client->out_len) {
			new_mask |= WL_EVENT_WRITABLE;
		}
		wl_event_source_fd_update(client->source, new_mask);
	}
	if ((mask & WL_EVENT_READABLE) != 0) {
		char buf[4096];
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n <= 0) {
			ipc_client_destroy(client);
			return 0;
		}
		ipc_client_handle_input(server, client, buf, (size_t)n);
	}
	return 0;
}

static int ipc_handle_accept(int fd, uint32_t mask, void *data) {
	struct server *server = data;
	if ((mask & WL_EVENT_READABLE) == 0) {
		return 0;
	}
	int cfd = accept(fd, NULL, NULL);
	if (cfd < 0) {
		return 0;
	}
	if (fcntl(cfd, F_SETFL, O_NONBLOCK | O_CLOEXEC) < 0) {
		close(cfd);
		return 0;
	}
	struct ipc_client *client = calloc(1, sizeof(*client));
	if (client == NULL) {
		close(cfd);
		return 0;
	}
	client->fd = cfd;
	client->server = server;
	client->source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->display), cfd, WL_EVENT_READABLE,
		ipc_client_handle_data, client);
	if (client->source == NULL) {
		close(cfd);
		free(client);
		return 0;
	}
	wl_list_insert(server->ipc_clients.prev, &client->link);
	/* a fresh client needs the current window list to draw the bar */
	ipc_send_window_list(server, client);
	return 0;
}

/* serialize + broadcast a window event to all connected clients */
void ipc_send_window_event(struct server *server, const char *event,
		struct toplevel *tl) {
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return;
	}
	cJSON_AddStringToObject(root, "event", event);
	if (tl != NULL) {
		cJSON_AddNumberToObject(root, "id", tl->id);
		cJSON_AddStringToObject(root, "app_id",
			tl->app_id != NULL ? tl->app_id : "");
	} else {
		/* focus cleared: id 0, no window */
		cJSON_AddNumberToObject(root, "id", 0);
		cJSON_AddStringToObject(root, "app_id", "");
	}
	char *json = cJSON_PrintUnformatted(root);
	if (json != NULL) {
		struct ipc_client *client;
		wl_list_for_each(client, &server->ipc_clients, link) {
			ipc_client_queue(client, json);
		}
		free(json);
	}
	cJSON_Delete(root);
}

/* send the current mapped windows; target NULL broadcasts to everyone */
static void ipc_send_window_list(struct server *server,
		struct ipc_client *target) {	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return;
	}
	cJSON_AddStringToObject(root, "event", "window_list");
	cJSON *arr = cJSON_CreateArray();
	cJSON_AddItemToObject(root, "windows", arr);
	struct toplevel *tl;
	wl_list_for_each(tl, &server->toplevels, link) {
		if (tl->xdg_toplevel->base == NULL ||
				!tl->xdg_toplevel->base->surface->mapped) {
			continue;
		}
		cJSON *w = cJSON_CreateObject();
		cJSON_AddNumberToObject(w, "id", tl->id);
		cJSON_AddStringToObject(w, "app_id",
			tl->app_id != NULL ? tl->app_id : "");
		cJSON_AddItemToArray(arr, w);
	}
	/* tell (new) bars which window currently has focus so the highlight
	 * shows immediately; 0 = nothing focused (or the focused window is
	 * hidden/unmapped, e.g. minimized) */
	int focused_id = 0;
	struct toplevel *focused = server->focused;
	if (focused != NULL && focused->xdg_toplevel->base != NULL &&
			focused->xdg_toplevel->base->surface->mapped) {
		focused_id = focused->id;
	}
	cJSON_AddNumberToObject(root, "focused_id", focused_id);
	char *json = cJSON_PrintUnformatted(root);
	if (json != NULL) {
		if (target != NULL) {
			ipc_client_queue(target, json);
		} else {
			struct ipc_client *client;
			wl_list_for_each(client, &server->ipc_clients, link) {
				ipc_client_queue(client, json);
			}
		}
		free(json);
	}
	cJSON_Delete(root);
}

/* create the Unix socket; returns false on failure (compositor keeps
 * running without a status bar) */
bool ipc_server_init(struct server *server, const char *path) {
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
	if (listen(fd, 8) < 0) {
		close(fd);
		return false;
	}
	server->ipc_fd = fd;
	server->ipc_source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->display), fd, WL_EVENT_READABLE,
		ipc_handle_accept, server);
	if (server->ipc_source == NULL) {
		close(fd);
		server->ipc_fd = -1;
		return false;
	}
	wl_list_init(&server->ipc_clients);
	wlr_log(WLR_INFO, "IPC socket listening on %s", path);
	return true;
}

void ipc_server_destroy(struct server *server) {
	struct ipc_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &server->ipc_clients, link) {
		ipc_client_destroy(client);
	}
	if (server->ipc_source != NULL) {
		wl_event_source_remove(server->ipc_source);
		server->ipc_source = NULL;
	}
	if (server->ipc_fd >= 0) {
		close(server->ipc_fd);
		server->ipc_fd = -1;
	}
}
