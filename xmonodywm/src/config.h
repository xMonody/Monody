/*
 * config.h - compile-time configuration for xmonodywm
 *
 * All the tunables live here: window decorations (rounded border), the
 * resize/move grab zone, the background blur behind transparent windows
 * and the compositor keyboard shortcuts.  Edit the values and rebuild.
 */

#ifndef XMONODYWM_CONFIG_H
#define XMONODYWM_CONFIG_H

#include <xkbcommon/xkbcommon.h>

#include <wlr/types/wlr_keyboard.h>

#define CONFIG_CURSOR_THEME    "Adwaita"      /* cursor theme  */
#define CONFIG_TITLEBAR_CURSOR "pointer"      /* cursor shown while hovering the title strip */
#define CONFIG_MOVE_CURSOR     "all-scroll"   /* cursor shown while dragging (moving) a window */

#define CONFIG_BORDER_WIDTH     1.5         /* 边框可见粗细 */
#define CONFIG_BORDER_RADIUS    8           /* 边框圆角 (px) */
#define CONFIG_TITLEBAR_HEIGHT  8           /* 移动窗口边框范围 */
#define CONFIG_EDGE_THICKNESS   8           /* 调整窗口大小边框范围 */

#define CONFIG_BORDER_OVERLAP    0.5          /* 边框在窗口内边缘宽度 */
#define CONFIG_MAXIMIZED_GAP     1            /* 边框和屏幕间距 */
#define CONFIG_MAXIMIZED_GAP_BAR 0.5          /* 边框和状态栏间距 */
#define CONFIG_BAR_TOP_OVERLAP   1            /* 窗口顶部相对状态栏的偏移(px) */

#define CONFIG_FULLSCREEN_BORDER_COLOR  0xFF87BEAAu /* 全屏窗口的边框颜色  */
#define CONFIG_FULLSCREEN_GAP    1                  // 全屏窗口与屏幕边缘的间距 (px)
#define CONFIG_MAXIMIZED_BORDER_ENABLED 1           /* 最大化时是否显示边框 */

#define CONFIG_BORDER_COLOR           0xFF676E95u   /* 0xFF7C73B0u  活动窗口 (#7C73B0) */
#define CONFIG_BORDER_COLOR_UNFOCUSED 0xFF676E95u   /*  非活动窗口(#676E95) */

/* popup (菜单/提示框/下拉框) 边框 */
#define CONFIG_POPUP_BORDER_WIDTH  1            /* popup 边框宽度 (px) */
#define CONFIG_POPUP_BORDER_COLOR  0xFF676E95u    /* popup 边框颜色, 0xAARRGGBB */

/* 不绘制光晕. 把大小设为 0 即可完全关闭. */
#define CONFIG_BORDER_GLOW_SIZE 18        /* 光晕超出边框的宽度 (px) */
#define CONFIG_BORDER_GLOW_ALPHA 0.18f    /* 光晕在边框边缘处的峰值不透明度 */

#define CONFIG_BORDER_COLOR_MIN 0xFFF5A3A3u    /* middle third: minimize/restore (#f5a3a3) */
#define CONFIG_BORDER_COLOR_MAX 0xFF87BEAAu /* left third: maximize (#87beaa) */
#define CONFIG_BORDER_COLOR_CLOSE 0xFFD55F6Fu  /* right third: close (#d55f6f) */

#define CONFIG_DOUBLE_CLICK_NS (400 * 1000000L) /* double-click window (ns) */
#define CONFIG_LONG_PRESS_NS (350 * 1000000L) /* holding the strip this long grabs the window (ns) */
#define CONFIG_DRAG_THRESHOLD 4.0         /* 判断是否移动窗口 */

#define CONFIG_WHEEL_DEBOUNCE_ENABLED false /* 控制是否启用骚鼠标 */
#define CONFIG_WHEEL_BURST_NS (800 * 1000000L)   /* one continuous scroll: max length = one action (0.8 s) */
#define CONFIG_WHEEL_TICK_GAP_NS (300 * 1000000L) /* two ticks this far apart = next action (0.3 s) */

#define CONFIG_BLUR_ENABLED true               /* 控制透明窗口是否启用阴影 */

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

#endif /* XMONODYWM_CONFIG_H */
