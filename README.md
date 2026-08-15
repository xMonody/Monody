# aiwm

一个最小的 **Wayland 浮动窗口合成器**（`xmonodywm`）+ 一个 **Win11 风格浮动状态栏**
（`xmonodybar`）。两者通过一个 Unix domain socket 用换行分隔的 JSON 通信。

- `xmonodywm` — 基于 **wlroots 0.19**、用 **C** 编写的浮动合成器，无自绘窗口装饰。
- `xmonodybar` — 基于 **Qt 6 + Qt Quick (QML) + wlr-layer-shell**（`layer-shell-qt`）的任务栏。

```
aiwm/
├── xmonodywm/   合成器（C + wlroots 0.19）
│   ├── src/     合成器源码
│   ├── Protocol/ Wayland 协议 XML
│   ├── xmonodywm/run  示例自动启动脚本
│   └── test-*.c 打包的测试客户端
├── xmonodybar/  状态栏（Qt 6 + QML + layer-shell-qt）
│   ├── src/     状态栏源码
│   ├── qml/     QML 界面
│   └── scripts/mock_compositor.py  模拟合成器
└── README.md
```

---

## Debian 依赖

以下包名基于 **Debian 13 (trixie)** 验证。Ubuntu 24.04+ 的包名基本一致。

### 公共构建工具

```sh
sudo apt install build-essential cmake pkg-config git
```

### 合成器 `xmonodywm`

```sh
sudo apt install \
  libwayland-bin libwayland-dev \
  libxkbcommon-dev libcjson-dev libpixman-1-dev libinput-dev \
  libegl-dev libgles2-mesa-dev libdrm-dev
```

| Debian 包 | 提供（pkg-config 模块 / 工具） |
|---|---|
| `libwayland-bin` | `wayland-scanner` |
| `libwayland-dev` | `wayland-server`、`wayland-client` |
| `libxkbcommon-dev` | `xkbcommon` |
| `libcjson-dev` | `libcjson` |
| `libpixman-1-dev` | `pixman-1` |
| `libinput-dev` | `libinput` |
| `libegl-dev` | `egl` |
| `libgles2-mesa-dev` | `glesv2` |
| `libdrm-dev` | wlroots 头文件传递依赖的 libdrm 头文件 |

**wlroots 0.19 需要自行编译。** Debian 13 只打包了 `libwlroots-0.18-dev`（0.18），
而本项目 `find_package` 要求 `wlroots-0.19`。用 meson/ninja 从源码构建并安装到
`/usr/local`（这就是合成器构建时为什么带
`PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig`）：

```sh
# wlroots 0.19 的构建依赖（除了上面已列出的库外还需要）：
sudo apt install meson ninja-build wayland-protocols \
  libgbm-dev liblcms2-dev libudev-dev libseat-dev \
  libdisplay-info-dev libliftoff-dev hwdata \
  libxcb1-dev libxcb-dri3-dev libxcb-present-dev libxcb-render0-dev \
  libxcb-render-util0-dev libxcb-shm0-dev libxcb-xfixes0-dev libxcb-xinput-dev

git clone https://gitlab.freedesktop.org/wlroots/wlroots.git -b 0.19
cd wlroots
meson setup build --prefix=/usr/local --buildtype=release
ninja -C build
sudo ninja -C build install
```

安装完成后确认 `pkg-config --modversion wlroots-0.19` 能输出 `0.19.x`。

### 状态栏 `xmonodybar`

```sh
sudo apt install qt6-base-dev qt6-declarative-dev qt6-wayland liblayershellqtinterface-dev
```

| Debian 包 | 用途 |
|---|---|
| `qt6-base-dev` | Qt6::Core / Gui / Network |
| `qt6-declarative-dev` | Qt6::Qml / Quick（QML 引擎） |
| `qt6-wayland` | Qt Wayland QPA 插件 + layer-shell shell 集成（运行时必需） |
| `liblayershellqtinterface-dev` | `LayerShellQt` 的 CMake 配置与头文件 |

可选（运行时图标渲染，缺省时自动降级）：

```sh
# 高质量 SVG 渲染：librsvg 通过 gdk-pixbuf 加载（推荐）
sudo apt install libgdk-pixbuf-2.0-0 librsvg2-common
# Qt 自带 SVG 渲染器（作为 gdk-pixbuf 不可用时的回退）
sudo apt install qt6-svg-plugins
```

> gdk-pixbuf 与 librsvg 在状态栏里是 **运行时 dlopen** 的（见
> `src/IconProvider.cpp`），缺失不会导致构建失败，只是 SVG 图标回退到 Qt 的渲染器。

### 运行时（可选，按需安装）

```sh
sudo apt install foot wlr-randr swaybg fcitx5 fcitx5-chinese-addons
```

`foot` 是默认快捷键打开的终端；`wlr-randr`/`swaybg` 用于输出与壁纸配置；
`fcitx5` + `fcitx5-chinese-addons` 用于中文输入法。

---

## 构建

### 合成器

```sh
cd xmonodywm
PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig \
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

产物：`build/xmonodywm`（以及 `build/test-*` 测试客户端）。

### 状态栏

```sh
cd xmonodybar
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

产物：`build/xmonodybar`。

---

## 运行

### 启动合成器

从 TTY（或带 seat 的会话）运行：

```sh
cd xmonodywm
./build/xmonodywm [-s 'startup command']
```

合成器启动后（后端就绪、Wayland socket 生效）会读取 **`~/.config/xmonodywm/run`**
（若设置了 `$XDG_CONFIG_HOME` 则为 `$XDG_CONFIG_HOME/xmonodywm/run`），每行一条命令，
经 `/bin/sh -c` 执行；每行自动后台化，空行与 `#` 注释被跳过，每行启动时记录日志。
每个子进程用 `setsid()` 脱离会话、stdin/stdout/stderr 重定向到 `/dev/null`，
并由 SIGCHLD 回收，避免僵尸进程。典型内容：

```sh
# 输出配置、壁纸、输入法
wlr-randr --output Virtual-1 --mode 2880x1800 --scale 1.5
swaybg -i ~/1.jpg -m fill
fcitx5 -d
```

`-s 'cmd'` 仍可作为一次性启动命令（在读取该文件之后执行）。

### 启动状态栏

在 Wayland 会话内（合成器必须实现 `wlr-layer-shell`）：

```sh
cd xmonodybar
./build/xmonodybar
```

状态栏默认连接 `$XDG_RUNTIME_DIR/xmonodywm.sock`（回退 `/tmp/xmonodywm.sock`），
可用 `--socket <path>` 覆盖。它会每秒钟自动重连，合成器重启后无需手动重启状态栏。

---

## `xmonodywm` — 合成器

### 设计要点

- 仅浮动窗口，无平铺、无标签页。
- **不自绘窗口装饰**：CSD 窗口保留原生控件；无装饰窗口只获得不可见的抓取区
  （顶部标题条 + 缩放边）。
- **CSD 窗口**（`CLIENT_SIDE` via xdg-decoration，或自带标题栏的应用）保留原生
  控件：客户端标题栏通过 `xdg_toplevel.move` 移动窗口，按钮正常，
  `request_maximize` / `request_minimize` 被尊重。
- **SSD / 无装饰窗口** 获得合成器拥有的隐形边框抓取区（左右下边与角、顶部标题条，
  标题条左/中/右三段分别映射 最小化 / 最大化-还原 / 关闭；光标样式随指针进入
  切换、离开即还原）：

  | 指针位置 | 光标 | 手势 | 动作 |
  |---|---|---|---|
  | 顶部 20 px 标题条 | `all-scroll` | 悬停 | 提示可拖动 |
  | 顶部 20 px 标题条 | `all-scroll` | 按住（左或右）+ 拖动 | 移动窗口 |
  | 顶部 20 px 标题条 | `grabbing` | 长按（约 350 ms） | 抓住窗口跟随光标 |
  | 标题条左 1/3 | `all-scroll` | 双击 | 最小化 |
  | 标题条中 1/3 | `all-scroll` | 双击 | 切换最大化 / 还原 |
  | 标题条右 1/3 | `all-scroll` | 双击 | 关闭 |
  | 窗口任意处 | （客户端） | 按住右键 + 滚轮上 | 切换最大化 / 还原 |
  | 窗口任意处 | （客户端） | 按住右键 + 滚轮下 | 切换最小化 / 还原 |
  | 窗口任意处 | （客户端） | 按住右键、双击左键 | 切换最大化 / 还原 |
  | 窗口任意处 | （客户端） | 按住左键、双击右键 | 关闭 |
  | 窗口任意处 | `grabbing` | 按住一个键再按住另一个 | 移动窗口（松开还原光标） |
  | 左/右边缘 | `ew-resize` | 按住 + 拖动 | 水平缩放 |
  | 底边缘 | `ns-resize` | 按住 + 拖动 | 垂直缩放 |
  | 底角 | `nwse/nesw` | 按住 + 拖动 | 对角线缩放 |

  点击窗口任意处聚焦并提升它。
- **弹出层（popup）优先于合成器边框**：菜单/下拉/工具提示打开时，其下的一切都
  属于弹出层 —— 合成器的缩放边与标题条在那里被禁用，悬停重叠在窗口边框上的菜单
  不会触发缩放，点击落在菜单上。
- **右键 + 滚轮与双键组合手势由 `CONFIG_WHEEL_DEBOUNCE_ENABLED` 门控**
  （默认关闭）。开启后，触控板一甩或高分辨率滚轮在短时间内会送达多个 tick，
  用两个阈值合并：一次连续滚动（无论多快）最多只算一次动作
  （`CONFIG_WHEEL_BURST_NS`，800 ms），且两次 tick 至少相隔
  `CONFIG_WHEEL_TICK_GAP_NS`（300 ms）才算下一次 —— 一甩只切换一次，慢速逐格滚动
  则每格触发一次；松开右键时重置。
- **拖动最大化窗口的顶部标题条会先还原** 到先前位置与尺寸，再继续拖动 —— 与
  Windows 一致。
- **拖动被 Windows 式钳制**：顶部 layer-shell 状态栏是硬边界，窗口顶边不能越过它；
  底部状态栏不阻挡拖动（光标保持在状态栏之上、窗口跟随无人工上限）。
- **光标尺寸不再由应用猜测**：实现 `cursor-shape-v1`，客户端选择形状后由合成器
  按输出精确分数比例从自己的 xcursor 主题渲染，1.75x 输出上合成器与客户端光标一致。

### 支持的协议

`wl_compositor`(v6) / `wl_subcompositor` / `wl_surface` / `wl_region` / `wl_seat` /
`wl_shm` / `zwp_linux_dmabuf_v1`(v5) / `xdg_wm_base`(v6) / `wp_viewporter` /
`wp_presentation`(v2) / `zwlr_layer_shell_v1`(v5) / `zxdg_decoration_manager_v1` /
`zwlr_output_manager_v1` / `zwlr_foreign_toplevel_manager_v1` /
`zwlr_virtual_pointer_manager_v1` / `wp_cursor_shape_manager_v1` / `xdg_activation_v1` /
`wp_fractional_scale_v1` / `wp_linux_drm_syncobj_manager_v1` / `zwp_input_method_v2` /
`zwp_text_input_v3`。

协议 XML 位于 `Protocol/`，CMake 构建时用 `wayland-scanner` 为每个文件生成
`<name>-protocol.h`（服务端头）、`<name>-client-protocol.h`（客户端头）与
`<name>-protocol.c`（编组代码）到 `build/protocol/`。合成器本身不链接这些代码
（wlroots 在服务端实现全局），生成的客户端代码供打包的测试客户端链接。

### 配置（`src/config.h`）

| 选项 | 含义 | 默认 |
|---|---|---|
| `CONFIG_EDGE_THICKNESS` | 窗口边/角的抓取区宽度（px） | `8` |
| `CONFIG_TITLEBAR_HEIGHT` | 窗口顶部标题条抓取区（px） | `8` |
| `CONFIG_LONG_PRESS_NS` | 长按此时间抓住窗口移动（ns） | `350 ms` |
| `CONFIG_WHEEL_DEBOUNCE_ENABLED` | 右键+滚轮：合并快速 tick | `false` |
| `CONFIG_WHEEL_BURST_NS` | 一次连续滚动 = 一次动作的最长时间 | `800 ms` |
| `CONFIG_WHEEL_TICK_GAP_NS` | 两次 tick 相隔此时间 = 下一次动作 | `300 ms` |
| `CONFIG_MOD_MAIN` / `CONFIG_KEY_*` | 快捷键修饰键 / 键值 | `Shift+Alt` |

### 快捷键（默认，`src/config.h` 中可改）

| 按键 | 动作 |
|---|---|
| `Shift+Alt+Q` | 退出合成器 |
| `Shift+Alt+Enter` | 切换最大化 / 还原 |
| `Shift+Alt+M` | 最小化聚焦窗口 |
| `Shift+Alt+N` | 聚焦下一个窗口（还原最小化窗口） |
| `Shift+Alt+P` | 聚焦上一个窗口（还原最小化窗口） |
| `Shift+Alt+C` | 关闭聚焦窗口 |
| `Shift+Alt+F` | 打开 `foot` 终端 |
| `Super+Q` | 退出合成器（遗留） |

快捷键由合成器消费，不会转发给客户端。

### 中文输入（fcitx5）

合成器实现输入法中继（`src/ime.c`），把 `zwp_input_method_v2`（fcitx5/ibus 连接）
与 `zwp_text_input_v3`（各窗口文本输入）串起来，支持 fcitx5 中文输入。启动 fcitx5：

```sh
fcitx5 -d
```

如果 fcitx5 已启动但一直停留在 *keyboard-us* 直通、无法输入中文，合成器没问题，
请检查：pinyin 在 fcitx5 分组里（`fcitx5-configtool` 或 `fcitx5-remote -s pinyin`），
以及拼音词典存在（`ls /usr/share/libime/pinyin.dict`）。某些系统 `libime-data`
缺词典会让 fcitx5 静默回退到直通；用
`sudo apt-get install --reinstall libime-data`（或重装 `fcitx5-chinese-addons`）修复。

### 测试

仓库内置一组 Wayland 测试客户端（由 `Protocol/` 生成的客户端头构建，不依赖 wlroots
构建树）：`test-client`、`test-interaction`（驱动虚拟指针遍历整套手势）、
`test-cursor`、`test-cursor-shape`、`test-select-drag`(+`.sh`)、
`test-resize-cursor`(+`.sh`)、`test-bar-clamp`、`test-ime-relay`、
`test-ime-app`（配合真实 fcitx5）等。无头运行：

```sh
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 ./build/xmonodywm \
  -s 'WAYLAND_DISPLAY=wayland-0 ./build/test-client; WAYLAND_DISPLAY=wayland-0 ./build/test-interaction'
```

---

## `xmonodybar` — 状态栏

### 特性

- 左侧 **开始图标**（Win 风格 logo）。
- 每个运行中的窗口一个图标，从左到右排列。
- **聚焦** 窗口获得圆角背景 + 下划线胶囊（Win11 风格）。
- 窗口全屏时整条状态栏 **隐藏**，退出全屏后恢复。
- **点击** 窗口图标向合成器发送激活消息。

### IPC 协议

合成器与状态栏通过 Unix socket 通信
（`$XDG_RUNTIME_DIR/xmonodywm.sock`，回退 `/tmp/xmonodywm.sock`），每行一个 JSON。

合成器 → 状态栏：

```json
{"event":"window_list","windows":[{"id":1,"app_id":"firefox"}],"focused_id":1}
{"event":"window_added","id":1,"app_id":"firefox"}
{"event":"window_removed","id":1}
{"event":"window_focus","id":1}
{"event":"window_full","id":3}
```

状态栏 → 合成器：

```json
{"action":"focus_window","id":1}
{"action":"list_windows"}
```

状态栏解析较宽松：接受无换行/拼接的 JSON 流；合成器重启后每秒自动重连并重新获取
`window_list`。右下角小圆点绿色表示已连接、红色表示断开（点击可切换调试面板）。

### 图标查找

按 `app_id` 依次搜索：

1. `$XDG_DATA_HOME/icons`（默认 `~/.local/share/icons`）与 `/usr/share/icons`
   - `hicolor/<size>/apps/<app_id>.{png,svg,svgz,xpm}`（尺寸 256…16）
   - `hicolor/scalable/apps/<app_id>.*` 及其它主题
2. `/usr/share/pixmaps/<app_id>.*` 与 `$XDG_DATA_HOME/pixmaps/<app_id>.*`

并尝试常见拼写变体（小写、`-` → `_`）。仍无匹配时回退 `.desktop` 文件
（basename / `StartupWMClass` / `Name` 命中则取其 `Icon=`）。最终仍无则画一个
带应用首字母的色块。

SVG 用 **librsvg via gdk-pixbuf**（运行时加载，Qt 自带 SVG 渲染器作为回退）渲染，
因为 Qt 内置渲染器会丢掉许多真实 SVG 的渐变引用/滤镜/裁剪路径。

### 调试

- `QT_LOGGING_RULES="bar.socket.debug=true"` — 打印每条收发 JSON。
- `BAR_DEBUG=1` — 屏幕内调试面板（socket 路径、连接状态、窗口数、最近事件）。

### 测试

```sh
cd xmonodybar
python3 scripts/mock_compositor.py
# mock> add 1 firefox
# mock> add 2 kitty
# mock> focus 2
# mock> full true
# mock> rm 1
```

它会打印状态栏发回的每条 JSON，点击图标可在终端里看到 `focus_window`。

---

## 源码布局

**`xmonodywm/src/`**

```
config.h    所有可调项：快捷键、抓取区
server.h    共享结构（server/toplevel/layer_surface）+ 跨模块 API
main.c      入口：display/backend/scene 设置、协议全局、运行循环
ipc.c/h     状态栏 socket（JSON over Unix domain socket）
scene.c     scene-graph 标记 / 命中测试辅助
toplevel.c  xdg-shell 窗口、窗口状态（max/min/fullscreen）
layer.c     wlr-layer-shell 表面 + 工作区
output.c    显示器、输出布局、wlr-output-management
input.c     seat、键盘、合成器快捷键
ime.c       输入法中继：zwp_input_method_v2 <-> zwp_text_input_v3（fcitx5/ibus）
pointer.c   光标交互（移动 / 缩放 / 标题条手势）
```

**`xmonodybar/src/` 与 `qml/`**

```
src/main.cpp            layer-shell 设置（顶层、锚点、独占区）
src/BarController.cpp   socket 客户端、窗口模型、图标查找
src/DesktopApps.cpp     .desktop 文件解析（启动菜单）
src/IconFinder.cpp      XDG 图标主题查找（返回 file:// URL）
src/IconProvider.cpp    image://icons provider；SVG 经 gdk-pixbuf 渲染
qml/main.qml            任务栏 UI（Win11 风格）+ 启动菜单
scripts/mock_compositor.py
```

---

## 各组件详细文档

- 合成器：见 [`xmonodywm/README.md`](xmonodywm/README.md)
- 状态栏：见 [`xmonodybar/README.md`](xmonodybar/README.md)
