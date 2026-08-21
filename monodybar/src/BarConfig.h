#pragma once

#include <QColor>

// ===========================================================================
// xmonodybar 外观配置
//
// 修改下面的变量并重新编译即可调整状态栏外观, 无需改动其他代码。
// 所有值通过 main.cpp 注入到 QML 上下文 (barCfg* 前缀), 供 qml/main.qml 使用。
// ===========================================================================

namespace barCfg {

// 状态栏位置: true = 顶部, false = 底部
inline constexpr bool atTop = false;

// 状态栏宽度 (像素), 0 或负数 = 铺满整个屏幕宽度
inline constexpr int width = 0;

// 状态栏高度 (像素), 同时作为 layer-shell 的排他区域 (exclusive zone)
inline constexpr int height = 38;

// 圆角弧度 (像素), 0 = 直角
inline constexpr int radius = 10;

// win 图标与第一个应用图标之间的距离 (像素)
inline constexpr int winAppGap = 30;

// 两个应用图标之间的距离 (像素)
inline constexpr int appGap = 18;

// 焦点/悬停背景超出应用图标的宽度 (单边, 像素, 左右对称)
// 例如: 图标宽 29, focusPad = 4 => 背景宽 29 + 2*4 = 37
// 背景高度固定为 36 (见 qml/main.qml), 不随 focusPad 变化。
inline constexpr int focusPad = 6;

// 焦点/悬停背景 (胶囊) 的圆角弧度 (像素)
inline constexpr int focusRadius = 6;

// 状态栏透明度, 0.0 = 全透明, 1.0 = 不透明
// (叠加在 barColor 之上, barColor 自带的 alpha 会被忽略)
inline constexpr qreal opacity = 0.9;

// 状态栏背景色 (RGB)
inline const QColor barColor = QColor(0x1e, 0x1e, 0x2e); //#1E1E2E

// 最后一个字节是透明度: 0x1a ≈ 10% (半透明), 0x00 = 全透明。
inline const QColor activeBg = QColor(0xff, 0xff, 0xff, 0x10);

// 弹框 (右键菜单) 背景色: 最后一个字节是透明度 (0xf5 ≈ 96%)。
inline const QColor menuBg = QColor(0x30, 0x34, 0x46, 0xf5);

// 活动 (聚焦) 窗口图标背景的边框宽度 (像素), 0 = 不画边框。
// 边框颜色/透明度跟随 activeBg 上面的白色 (见 LiquidGlass.qml)。
inline constexpr int activeBorder = 0;

} // namespace barCfg
