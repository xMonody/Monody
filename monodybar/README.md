# xmonodybar

A floating taskbar (status bar) for Wayland compositors, written in
**Qt 6 + Qt Quick (QML) + wlr-layer-shell** (`layer-shell-qt`).

It looks and behaves like the Windows 11 taskbar:

- a **left icon** (drawn Windows-style logo) — no function yet
- one **icon per running window**, added left-to-right
- **focused** window gets a rounded background + underline pill (Win11 style)
- the whole bar **hides when a window goes fullscreen** and reappears afterwards
- **clicking** a window icon sends an activation message to the compositor

## Protocol

The compositor and the bar talk over a unix socket
(`$XDG_RUNTIME_DIR/xmonodywm.sock`, falling back to `/tmp/xmonodywm.sock`;
override with `--socket <path>`), one JSON object per line.

Compositor → bar:

```json
{"event":"window_list","windows":[{"id":1,"app_id":"firefox","pid":1234}],"focused_id":1}  // snapshot on connect; focused_id optional (0 = none focused)
{"event":"window_added","id":1,"app_id":"firefox","pid":1234}   // draw icon at the end of the taskbar
{"event":"window_removed","id":1}                                     // remove the icon
{"event":"window_focus","id":1}                                       // highlight the icon (id 0 clears)
{"event":"window_full","id":3}                                        // sent once on enter AND once on exit fullscreen
```

Bar → compositor:

```json
{"action":"focus_window","id":1}   // sent when an icon is clicked
{"action":"close_window","id":1}    // context menu: close
{"action":"maximize_window","id":1} // context menu: toggle maximize
{"action":"minimize_window","id":1} // context menu: minimize
{"action":"list_windows"}            // sent right after connecting; answer with a
                                       // fresh window_list (with focused_id) so the
                                       // focused icon shows immediately
```

If the snapshot includes `focused_id` (or a per-window `focused` flag) the bar
highlights the focused icon right away; otherwise the icon stays unhighlighted
until the first `window_focus` event arrives.

The parsing is deliberately lenient: it also accepts JSON streams without
newlines and concatenated objects, and it reconnects automatically every
second if the compositor restarts (a fresh `window_list` is then applied).

## Icons

When the bar starts it scans the XDG `.desktop` files and the icon themes
**once** and keeps the results in memory as plain hash lookups, so drawing
an icon never touches the filesystem again:

- `Exec=` first token → icon path
- desktop file id / `StartupWMClass` / `Name` → icon path (used for the
  taskbar `app_id`s, e.g. Qt Creator's `qtcreator` finds its icon stored as
  `QtProject-qtcreator.png`)
- themed icon name → icon path (tray / attention icons)

`Icon=` values are resolved against the icon themes in this order:
`$XDG_DATA_HOME/icons` (default `~/.local/share/icons`) first, then
`/usr/share/icons`, then `/usr/local/share/icons` (the flat pixmap dirs are a
last resort). Within a theme larger sizes win, then `scalable`/`symbolic`
dirs, and common spelling variants are tried too (lowercase, `-` → `_`).
The user theme's definitions win over the system themes' defaults: its real
SVG icons and its symlink aliases alike - e.g. WhiteSur's
`links/apps/scalable/nvim.svg → neovim.svg` resolves `nvim` to WhiteSur's
`neovim.svg`, `vim`/`gvim` to `vim.svg`, `foot` to `terminal.svg`. The themed
icon name is preferred over the `.desktop` `Icon=` mapping, which is only a
fallback. If nothing is found a colored tile with the app's initial is
drawn instead.

Icons are served to QML through an `image://icons/...` image provider
(`IconProvider`). SVG files are rendered with **librsvg via gdk-pixbuf**
(loaded at runtime, with Qt's own SVG renderer as fallback), because Qt's
built-in SVG renderer silently drops parts of many real-world SVGs
(Inkscape gradient references, filters, clip paths, ...) - e.g. the Alacritty
logo lost its bright gradient parts and looked broken on the taskbar.

The socket reconnects automatically every second if the compositor restarts.
A small dot in the bottom-right corner is green when connected, red otherwise.

## Build

```bash
# deps (Debian/Ubuntu): qt6-base-dev, qt6-declarative-dev, qt6-wayland,
#                       libqt6network6, liblayershellqtinterface-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The layer-shell-qt **dev** package (`liblayershellqtinterface-dev`) provides the
public headers and the CMake package config, so the project uses
`find_package(LayerShellQt)` / `LayerShellQt::Interface` instead of vendoring
headers.

## Run

Run it inside your Wayland session (the compositor must implement
`wlr-layer-shell`, e.g. sway / hyprland / river / wayfire):

```bash
./build/xmonodybar
```

`Shell::useLayerShell()` switches the Qt Wayland shell integration to
`layer-shell`; the window is anchored top/left/right with a 48 px exclusive
zone, so regular windows are tiled below the bar.

Debugging: run with `QT_LOGGING_RULES="bar.socket.debug=true"` to see every
received/sent JSON message, or `BAR_DEBUG=1` to show an on-screen debug panel
(socket path, connection state, window count, last event). Click the small
green/red dot in the corner to toggle the panel at runtime.

## Test

`scripts/mock_compositor.py` simulates the compositor side:

```bash
python3 scripts/mock_compositor.py
# mock> add 1 firefox
# mock> add 2 kitty
# mock> focus 2
# mock> full true
# mock> rm 1
```

It prints every JSON line the bar sends back, so clicking an icon is visible
in the terminal.

## Project layout

```
src/main.cpp            layer-shell setup (top layer, anchors, exclusive zone)
src/BarController.cpp   socket client, window model
src/IconIndex.cpp      one-time icon index (exec/app_id/name -> icon path)
src/IconProvider.cpp   image://icons provider; SVG rendering via gdk-pixbuf
qml/main.qml            taskbar UI (Win11 style)
scripts/mock_compositor.py
```
