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

/* ------------------------------------------------------------------ */
/* window decorations                                                  */
/* ------------------------------------------------------------------ */

#define CONFIG_BORDER_WIDTH 2           /* server-side border stroke width (px) */
#define CONFIG_BORDER_RADIUS 12         /* rounded-corner radius of the border (px) */
#define CONFIG_BORDER_COLOR 0xFF7C73B0u /* border color, 0xAARRGGBB (#7C73B0) */

/* ------------------------------------------------------------------ */
/* window interaction                                                  */
/* ------------------------------------------------------------------ */

#define CONFIG_TITLEBAR_HEIGHT 20         /* invisible title strip at the window top (px) */
#define CONFIG_EDGE_THICKNESS 20          /* grab zone on window edges/corners for move+resize (px) */
#define CONFIG_DOUBLE_CLICK_NS (400 * 1000000L) /* double-click window (ns) */
#define CONFIG_DRAG_THRESHOLD 8.0         /* px before a press becomes a drag */

/* ------------------------------------------------------------------ */
/* background blur                                                     */
/* ------------------------------------------------------------------ */

/* gaussian blur behind transparent windows (terminal emulators): when
 * true a window whose buffer contains semi-transparent pixels gets a
 * GLSL-blurred backdrop; when false transparent windows just show the
 * sharp scene behind them. */
#define CONFIG_BLUR_ENABLED true

/* ------------------------------------------------------------------ */
/* keyboard shortcuts                                                  */
/* ------------------------------------------------------------------ */

/* modifier combos, bitmasks of enum wlr_keyboard_modifier */
#define CONFIG_MOD_MAIN (WLR_MODIFIER_SHIFT | WLR_MODIFIER_ALT) /* Shift+Alt */
/* alternative combo for the quit binding: Super+KEY_QUIT (the main combo
 * works for quit as well) */
#define CONFIG_MOD_QUIT (WLR_MODIFIER_LOGO)

/* keysyms, see /usr/include/xkbcommon/xkbcommon-keysyms.h */
#define CONFIG_KEY_QUIT XKB_KEY_q
#define CONFIG_KEY_MAXIMIZE XKB_KEY_Return
#define CONFIG_KEY_MINIMIZE XKB_KEY_m
#define CONFIG_KEY_NEXT_WINDOW XKB_KEY_n
#define CONFIG_KEY_PREV_WINDOW XKB_KEY_p
#define CONFIG_KEY_CLOSE XKB_KEY_c
#define CONFIG_KEY_TERMINAL XKB_KEY_f

#endif /* XMONODYWM_CONFIG_H */
