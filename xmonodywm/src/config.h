/*
 * config.h - compile-time configuration for xmonodywm
 *
 * All the tunables live here: the resize/move grab zone and the compositor
 * keyboard shortcuts.  Edit the values and rebuild.
 */

#ifndef XMONODYWM_CONFIG_H
#define XMONODYWM_CONFIG_H

#include <xkbcommon/xkbcommon.h>

#include <wlr/types/wlr_keyboard.h>

#define CONFIG_CURSOR_THEME    "Adwaita"      /* cursor theme  */
#define CONFIG_TITLEBAR_CURSOR "pointer"      /* cursor shown while hovering the title strip */
#define CONFIG_MOVE_CURSOR     "all-scroll"   /* cursor shown while dragging (moving) a window */

#define CONFIG_ROUNDED_RADIUS    8           /* 窗口圆角半径 (px) */
#define CONFIG_BORDER_WIDTH      1.5           /* 窗口边框宽度 (px) */

#define CONFIG_BORDER_FOCUSED    0x9F6680    /* 有焦点边框颜色 0xRRGGBB */
#define CONFIG_BORDER_UNFOCUSED  0x9F6680    /* 无焦点边框颜色 0xRRGGBB */

#define CONFIG_BORDER_TOP_LEFT   0xea9a71    /* 顶部边框左1/3颜色 (最小化) */
#define CONFIG_BORDER_TOP_MID    0xbf7e80    /* 顶部边框中1/3颜色 (最大化) */
#define CONFIG_BORDER_TOP_RIGHT  0xd55f6f    /* 顶部边框右1/3颜色 (关闭) */
#define CONFIG_BORDER_GRADIENT_WIDTH 10       /* 顶部边框三段颜色拼接处渐变宽度 (px) */

#define CONFIG_FULLSCREEN_BORDER 1            /* 全屏窗口是否显示边框 0/1 */
#define CONFIG_FULLSCREEN_BORDER_COLOR 0xea9a71 /* 全屏边框颜色 0xRRGGBB */

#define CONFIG_SHADOW_WIDTH      20          /* 窗口阴影宽度 (px) */
#define CONFIG_SHADOW_ALPHA      0.2f        /* 窗口阴影不透明度 0.0-1.0 */

#define CONFIG_TITLEBAR_HEIGHT  8           /* 移动窗口标题栏范围 */
#define CONFIG_EDGE_THICKNESS   8           /* 调整窗口大小边框范围 */

#define CONFIG_DOUBLE_CLICK_NS (400 * 1000000L) /* double-click window (ns) */
#define CONFIG_LONG_PRESS_NS (350 * 1000000L) /* holding the strip this long grabs the window (ns) */
#define CONFIG_DRAG_THRESHOLD 4.0         /* 判断是否移动窗口 */

#define CONFIG_WHEEL_DEBOUNCE_ENABLED true /* 控制是否启用骚鼠标 */
#define CONFIG_WHEEL_BURST_NS (800 * 1000000L)   /* one continuous scroll: max length = one action (0.8 s) */
#define CONFIG_WHEEL_TICK_GAP_NS (300 * 1000000L) /* two ticks this far apart = next action (0.3 s) */

/* 新窗口放置 (place.c): 创建时大小完全尊重客户端, 位置在屏幕上左右上下居中.
 * 默认以整个屏幕为基准严格居中; 设为 1 时改为以工作区为基准
 * (屏幕减去 layer-shell 状态栏的独占区), 保证新窗口不被状态栏盖住. */
#define CONFIG_CENTER_AVOID_BARS 0

#define CONFIG_MOD_MAIN (WLR_MODIFIER_SHIFT | WLR_MODIFIER_ALT) /* Shift+Alt */
#define CONFIG_MOD_QUIT (WLR_MODIFIER_LOGO)
#define CONFIG_MOD_TASK WLR_MODIFIER_ALT /* 可改为 WLR_MODIFIER_ALT */

/* keysyms, see /usr/include/xkbcommon/xkbcommon-keysyms.h */
#define CONFIG_KEY_QUIT XKB_KEY_q            // quit
#define CONFIG_KEY_MAXIMIZE XKB_KEY_Return   // max
#define CONFIG_KEY_MINIMIZE XKB_KEY_m        // mini
#define CONFIG_KEY_NEXT_WINDOW XKB_KEY_n     // next app
#define CONFIG_KEY_PREV_WINDOW XKB_KEY_p     // prev app
#define CONFIG_KEY_CLOSE XKB_KEY_c           // close app
#define CONFIG_KEY_CLOSE_OTHER XKB_KEY_x      // close other apps

#define MODKEY1 WLR_MODIFIER_LOGO                        // win
#define MODKEY2 WLR_MODIFIER_SHIFT | WLR_MODIFIER_ALT    // shift+alt
#define MODKEY3 WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL   // shift+ctrl
#define MODKEY WLR_MODIFIER_ALT   | WLR_MODIFIER_CTRL   // alt+ctrl

/* 应用启动快捷键: mods + key 启动 app，args 可为 NULL 或空串 */
struct config_app_shortcut {
	uint32_t mods;
	xkb_keysym_t key;
	const char *app;
	const char *args;
};

static const struct config_app_shortcut config_app_shortcuts[] = {
	{ MODKEY, XKB_KEY_f, "foot",    NULL },
	{ MODKEY, XKB_KEY_c, "firefox", NULL },
	{ MODKEY, XKB_KEY_w, "wezterm", NULL },
	{ MODKEY, XKB_KEY_k, "kitty",   NULL },
	{ MODKEY, XKB_KEY_q, "qq",      NULL },
};

/* 强制「无装饰」窗口（合成器接管边框）：这些客户端自己画 CSD，但它们的
 * 边缘调整依赖合成器，所以由合成器提供 resize 边缘 + resize 光标，并忽略
 * 它们自己的 resize 光标形状。
 * 匹配是大小写不敏感的 app_id 前缀匹配（xdg_toplevel.set_app_id），所以
 * 一条前缀能覆盖多个变体（如 "org.chromium" 覆盖 org.chromium.Chromium）。
 * 以后装了新应用（例如 VS Code）直接在这里加一行前缀即可，不用改 toplevel.c。 */
static const char *const config_force_undecorated[] = {
	"qq",
	"chromium",
	"google-chrome",
	"firefox",
	"org.chromium",
	"org.mozilla",
	"code",                 /* VS Code / VS Codium */
	"com.visualstudio.code", /* 官方 VS Code */
	NULL,
};

#endif /* XMONODYWM_CONFIG_H */
