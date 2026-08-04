# xmonodywm

A minimal floating Wayland compositor written in C on top of **wlroots 0.19**.

## Design

* Floating windows only — no tiling, no tabs.
* The compositor draws a **rounded border** (`#7C73B0`, 2 px stroke, 12 px
  corner radius) around undecorated windows; client-side decorated windows keep
  their native decorations.
* **Client-side decorated** windows (mode `CLIENT_SIDE` via xdg-decoration, or
  apps with their own header bars) keep their native controls: the client's own
  title bar moves the window through `xdg_toplevel.move`, its buttons work, its
  edges resize natively, and `request_maximize` / `request_minimize` are honored.
* **Server-side / undecorated** windows get an *invisible* frame owned by the
  compositor (the top 20 px and the 20 px edge/corner strips are grabbed
  zones; the cursor style updates as soon as the pointer enters them):

  | pointer position | cursor | gesture (left or right button) | action |
  |---|---|---|---|
  | top 20 px          | `move`        | hold + drag              | move the window |
  | top 20 px          | `move`        | hold + wheel up          | maximize |
  | top 20 px          | `move`        | hold + wheel down        | minimize |
  | top 20 px          | `move`        | double click             | close the window |
  | left/right edge    | `ew-resize`   | hold + drag              | resize horizontally |
  | bottom edge        | `ns-resize`   | hold + drag              | resize vertically |
  | bottom corners     | `nwse/nesw`   | hold + drag              | resize diagonally |

  Clicking anywhere on a window focuses and raises it.
* **Dragging the top strip of a maximized window restores it** to its previous
  position and size, then the drag continues with the cursor gripping the
  restored title bar — same as Windows.

## Protocols

| protocol | notes |
|---|---|
| `wl_compositor` (v6) | with `wl_subcompositor` |
| `wl_surface` / `wl_region` | part of the compositor |
| `wl_seat` | pointer + keyboard |
| `wl_shm` | via `wlr_shm_create_with_renderer` |
| `zwp_linux_dmabuf_v1` (v5) | via `wlr_linux_dmabuf_v1_create_with_renderer` |
| `xdg_wm_base` (v6) | toplevels, popups, move/resize/maximize/minimize requests |
| `wp_viewporter` | |
| `wp_presentation` (v2) | frame callbacks via `wlr_scene_output_send_frame_done` |
| `zwlr_layer_shell_v1` (v5) | background/bottom/top/overlay layers |
| `zxdg_decoration_manager_v1` | client requests honored; default `CLIENT_SIDE` |
| `zwlr_output_manager_v1` | apply/test + config broadcast |
| `zwlr_foreign_toplevel_manager_v1` | title/app_id/state + requests |
| `zwlr_virtual_pointer_manager_v1` | extra, used for input testing |

## Layout

```
layers (bottom -> top):
  background < bottom < toplevels < top < overlay
```

Layer-shell exclusive zones shrink the work area that maximized windows use.

## Build

The installed wlroots package in this environment doesn't ship the generated
protocol headers (e.g. `wlr-layer-shell-unstable-v1-protocol.h`), so point
meson at the wlroots build directory that contains them:

```sh
PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig \
meson setup build -Dwlroots_protocol_dir=/home/roots/wlroots-0.19.3/build/protocol
ninja -C build
```

Run (from a TTY / with a seat):

```sh
./build/xmonodywm [-s 'startup command']
```

## Shortcuts

| keys            | action                     |
|---|---|
| `Shift+Alt+Q`    | quit the compositor        |
| `Shift+Alt+Enter`| toggle maximize / restore the focused window |
| `Shift+Alt+M`    | minimize the focused window |
| `Shift+Alt+N`    | focus the next window (minimized windows are restored) |
| `Shift+Alt+P`    | focus the previous window (minimized windows are restored) |
| `Shift+Alt+C`    | close the focused window      |
| `Shift+Alt+F`    | open a `foot` terminal     |
| `Super+Q`        | quit the compositor (legacy) |

Notes:

* Maximizing remembers the floating geometry; pressing `Shift+Alt+Enter`
  again restores the previous size and position.
* Minimizing only hides the window, so it reappears at its original position
  when restored (via the focus cycle or a foreign-toplevel activate).
* Closing or minimizing the focused window hands keyboard/cursor focus to the
  previous visible window (or clears it when no other window is shown).

Shortcuts are consumed by the compositor and not forwarded to clients.

## IPC (status bar)

The compositor exposes a Unix domain socket at
`$XDG_RUNTIME_DIR/xmonodywm.sock` (fallback `/tmp/xmonodywm.sock`). Status
bars connect and receive **newline-delimited JSON** messages. Each window is
identified by a stable `id` assigned by the compositor; `app_id` is the
client-provided application id (e.g. `firefox`).

Events sent to every connected client:

```json
{"event":"window_added","id":1,"app_id":"firefox"}
{"event":"window_focus","id":1,"app_id":"firefox"}
{"event":"window_full","id":1,"app_id":"firefox"}
{"event":"window_removed","id":1,"app_id":"firefox"}
{"event":"window_focus","id":0,"app_id":""}
```

* `window_added` – a window was mapped (id/app_id of the new window).
* `window_removed` – a window was destroyed (id/app_id of the closed window).
* `window_focus` – focus changed; `id` is the newly focused window, or `0`
  when nothing is focused anymore.
* `window_full` – a window entered or left fullscreen.

On connect, and in reply to a client request
`{"action":"list_windows"}`, the compositor sends the current window list:

```json
{"event":"window_list","windows":[{"id":1,"app_id":"firefox"}]}
```

Requests a client can send (one JSON object per line):

| request | effect |
|---|---|
| `{"action":"list_windows"}` | reply with the current `window_list` |
| `{"action":"focus_window","id":2}` | focus (and restore if minimized) window 2; a `window_focus` event follows |
| `{"action":"close_window","id":2}` | request window 2 to close; `window_removed` follows |

A taskbar can use this to switch focus when an icon is clicked.


## Tests

The repo contains two small Wayland clients used to validate the compositor
(the generated protocol headers they include live in the wlroots build dir):

* `test-client.c` — xdg-shell configure/maximize/minimize/move and
  xdg-decoration mode negotiation.
* `test-interaction.c` — drives a virtual pointer to exercise the whole
  gesture set: drag-move, wheel maximize/minimize, double-click close,
  client-side-decoration pass-through, edge resize, and Windows-style
  restore-from-maximize.

The compositor is exercised headless with:

```sh
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 ./build/xmonodywm
```

and tested with `foot`, `swaybg` (layer-shell) and `wlr-randr`
(output-management).
