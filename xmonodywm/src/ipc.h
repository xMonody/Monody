/*
 * ipc.h - status-bar IPC socket (JSON over a Unix domain socket)
 */

#ifndef XMONODYWM_IPC_H
#define XMONODYWM_IPC_H

#include "server.h"

/* create the Unix socket; returns false on failure (compositor keeps
 * running without a status bar) */
bool ipc_server_init(struct server *server, const char *path);

void ipc_server_destroy(struct server *server);

/* serialize + broadcast a window event to all connected clients */
void ipc_send_window_event(struct server *server, const char *event,
	struct toplevel *tl);

#endif /* XMONODYWM_IPC_H */
