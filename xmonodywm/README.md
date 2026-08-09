# xmonodywm

A minimal floating Wayland compositor written in C on top of **wlroots 0.19**.

## Design

* Floating windows only — no tiling, no tabs.
* The compositor draws a **rounded border** (`#7C73B0`, 2 px stroke, 12 px
  corner radius) around undecorated windows; client-side decorated windows keep
  their native decorations.  The border is rendered on the GPU by a GLES2
  fragment shader (a rounded-rect SDF) drawn into a wlroots render-target
  buffer, so resizing costs no CPU rasterization.
* **Transparent windows get a gaussian-blurred backdrop** (frosted glass).
  When a window commits a buffer that actually contains semi-transparent
  pixels (terminal emulators with a transparent background, e.g. `foot`,
  `kitty`, `alacritty`), the compositor renders the scene *behind* the window
  into a private snapshot (a `wlr_scene_output_build_state` pass with its own
  swapchain), runs a separable gaussian blur on the GPU — two GLES2 passes,
  horizontal then vertical, on a 3x-downscaled intermediate — and places the
  result behind the window content, so the backdrop stays sharp-free and
  tracks moving windows, wallpaper and the window itself.  The blur is purely
  GLSL, same as the rounded border; opaque windows are never touched.
* **Client-side decorated** windows (mode `CLIENT_SIDE` via xdg-decoration, or
  apps with their own header bars) keep their native controls: the client's own
  title bar moves the window through `xdg_toplevel.move`, its buttons work, its
  edges resize natively, and `request_maximize` / `request_minimize` are honored.
* **Server-side / undecorated** windows get an *invisible* frame owned by the
  compositor (the top 20 px of the window and the 20 px strips *around* the
  left/right/bottom edges and corners are grabbed zones; the cursor style
  updates as soon as the pointer enters them and reverts as soon as it
  crosses back into the window):

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
| `zwp_input_method_v2` | input method (fcitx5/ibus) — activation, keyboard grab, preedit/commit |
| `zwp_text_input_v3` | per-window text input — enter/leave, surrounding text, commit string |

## Source layout

`main.c` was split into small modules under `src/`:

```
src/
  config.h    all tunables: shortcuts, border radius, blur toggle, edge grab zone
  server.h    shared structs (server/toplevel/layer_surface) + cross-module API
  main.c      entry point: display/backend/scene setup, protocol globals, run loop
  ipc.c/h     status-bar socket (JSON over a Unix domain socket)
  scene.c     scene-graph tagging / hit-testing helpers
  toplevel.c  xdg-shell windows, window state (max/min/fullscreen), decorations
  border.c    rounded server-side border for undecorated windows
  blur.c      GLSL gaussian background blur for transparent windows
  layer.c     wlr-layer-shell surfaces + work area
  output.c    monitors, output layout, wlr-output-management
  input.c     seat, keyboard, compositor shortcuts
  ime.c       input method relay: zwp_input_method_v2 <-> zwp_text_input_v3
              (fcitx5 / ibus Chinese input)
  pointer.c   cursor interaction (move / resize / title-bar gestures)
```

## Chinese input (fcitx5)

The compositor implements the **input method relay** so fcitx5 (or ibus)
can type Chinese.  The two protocols involved are wired together in
`ime.c`:

* `zwp_input_method_unstable_v2` — fcitx5 connects as the input method;
  the relay activates it when a surface gains keyboard focus and forwards
  its preedit/commit state to the focused window.
* `zwp_text_input_unstable_v3` — each focused window exposes its text input;
  surrounding text and content type flow to the input method, and the
  committed string flows back into the window.

While fcitx5 holds the **keyboard grab** (it does this while composing),
raw key events are forwarded to it instead of the focused client, and its
candidate window (an input popup surface) is shown in the overlay layer
following the cursor.

Start fcitx5 before the applications you want to type into:

```sh
fcitx5 -d
```

The relay is verified end to end by `test-ime-relay` (synthetic app + IM)
and by driving a real fcitx5 with the bundled `test-ime-app` + a virtual
keyboard.  If fcitx5 starts but stays in *keyboard-us* passthrough instead
of composing Chinese, the compositor is fine — check that:

* pinyin is in fcitx5's group (`fcitx5-configtool`, or
  `fcitx5-remote -s pinyin` with an active window), and
* the pinyin dictionary exists (`ls /usr/share/libime/pinyin.dict`).
  On some systems the `libime-data` package ships without it, which makes
  fcitx5 silently fall back to passthrough; fix with
  `sudo apt-get install --reinstall libime-data` (or reinstall
  `fcitx5-chinese-addons`).

## Protocols

Every protocol this project speaks lives as an XML description in
`Protocol/` (the core `wayland.xml` plus the wayland-protocols and wlroots
protocols — 13 files, including the input-method and text-input protocols
needed for fcitx5).  At build time CMake runs `wayland-scanner` over each
file and emits three artifacts into `build/protocol/`:

* `<name>-protocol.h`         (server-side header; wlroots' installed headers
                               `#include` these, e.g. `wlr_layer_shell_v1.h`)
* `<name>-client-protocol.h`  (client-side header)
* `<name>-protocol.c`         (marshalling code)

The compositor itself links none of that code: as a wlroots compositor,
wlroots implements the globals server-side.  The generated client code is
what the bundled test clients link against, so the tests are fully
self-contained.  To add a protocol: drop the `.xml` into `Protocol/`, add
its name to the `PROTOCOLS` list in `CMakeLists.txt` and (for tests)
include the generated client header.


## Layout

```
layers (bottom -> top):
  background < bottom < toplevels < top < overlay
```

Layer-shell exclusive zones shrink the work area that maximized windows use.

## Build

Built with **CMake**.  The installed wlroots package doesn't ship the
generated protocol headers it `#include`s internally; they are generated
from our own `Protocol/*.xml` into the build tree, so no external protocol
build directory is needed:

```sh
PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig \
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run (from a TTY / with a seat):

```sh
./build/xmonodywm [-s 'startup command']
```

**Autostart** — once the compositor is up (backend started, Wayland socket
live) it reads the user's startup commands from **`~/.config/mywm/run`**
(`$XDG_CONFIG_HOME/mywm/run` if set) and launches them one per line through
`/bin/sh -c`, so `~`, $VARS, quotes and shell syntax all work. **Every line
is backgrounded automatically** (an explicit trailing `&` is optional and
handled without double-`&` errors), blank lines and `#` comments are
skipped (trailing comments too), and each line is logged at startup.
Every spawned process is **detached the standard way**: `setsid()` puts it
in its own session without a controlling terminal (no Ctrl+C / SIGHUP from
the tty, survives the terminal going away), stdin/stdout/stderr are
redirected to `/dev/null` (no output polluting the compositor's tty, no
blocking on terminal I/O), and helpers are reaped via SIGCHLD so they never
pile up as zombies. Typical contents:

```sh
# output configuration, wallpaper, input method
wlr-randr --output Virtual-1 --mode 2880x1800 --scale 1.5
swaybg -i ~/1.jpg -f full
fcitx5 -d
```

`-s 'cmd'` still works as a one-off startup command (run after the file).

## Configuration

Everything tunable lives in **`src/config.h`** (edit and rebuild):

| option | meaning | default |
|---|---|---|
| `CONFIG_BORDER_RADIUS` | rounded-corner radius of the border (px) | `12` |
| `CONFIG_BORDER_WIDTH` / `CONFIG_BORDER_COLOR` | border stroke / color (`0xAARRGGBB`) | `2` / `#7C73B0` |
| `CONFIG_EDGE_THICKNESS` | grab zone on window edges/corners for move+resize (px) | `20` |
| `CONFIG_TITLEBAR_HEIGHT` | invisible title strip at the window top (px) | `20` |
| `CONFIG_BLUR_ENABLED` | `true`: GLSL gaussian blur behind transparent windows, `false`: sharp backdrop | `true` |
| `CONFIG_MOD_MAIN` / `CONFIG_KEY_*` | shortcut modifier combo / keysyms (see below) | `Shift+Alt` |

## Shortcuts

All key bindings (modifier combos and keysyms) are defined in
**`src/config.h`** — edit it and rebuild.  Defaults:

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

The repo contains two small Wayland clients used to validate the compositor;
CMake builds them from the client headers generated out of `Protocol/` (no
dependency on a wlroots build tree):

* `test-client.c` — xdg-shell configure/maximize/minimize/move and
  xdg-decoration mode negotiation.
* `test-interaction.c` — drives a virtual pointer to exercise the whole
  gesture set: drag-move, wheel maximize/minimize, double-click close,
  client-side-decoration pass-through, edge resize, and Windows-style
  restore-from-maximize.

* `test-ime-relay.c` — drives the input method relay end to end: a fake
  app (text-input-v3) plus a fake input method (input-method-v2) verify
  that focus activates the IM, that the IM receives the keymap and key
  events through the keyboard grab, and that its preedit/commit string
  reaches the app.
* `test-ime-app.c` — real-app side of the fcitx5 test: opens a toplevel
  with text input enabled, drives a virtual keyboard (so it also needs the
  `virtual-keyboard-unstable-v1` protocol) and prints whatever the input
  method commits; run it with a real fcitx5 to type Chinese.

The compositor is exercised headless with:

```sh
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 ./build/xmonodywm \
  -s 'WAYLAND_DISPLAY=wayland-0 ./build/test-client; WAYLAND_DISPLAY=wayland-0 ./build/test-interaction'
```
