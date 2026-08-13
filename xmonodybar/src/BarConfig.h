#pragma once

#include <QColor>

// ===========================================================================
// qt6-bar 外观配置
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

// 状态栏透明度, 0.0 = 全透明, 1.0 = 不透明
// (叠加在 barColor 之上, barColor 自带的 alpha 会被忽略)
inline constexpr qreal opacity = 0.9;

// 活动 (聚焦) 窗口图标是否显示背景
inline constexpr bool activeBgEnabled = false;

// 状态栏背景色 (RGB)
inline const QColor barColor = QColor(0x20, 0x20, 0x20);

// 活动 (聚焦) 窗口图标的背景色
inline const QColor activeBg = QColor(0x4d, 0x4d, 0x4d, 0x59);

// 聚焦窗口图标下方的下划线宽度 (像素)
inline constexpr int underlineWidth = 3;

// 聚焦窗口图标下方的下划线高度 (像素)
inline constexpr int underlineHeight = 3;

// 下划线与图标底部的距离 (像素): 正数向上, 负数向下
inline constexpr int underlineOffset = 0;

// 聚焦窗口图标下方的下划线颜色
inline const QColor underlineColor = QColor(0x9c, 0xcc, 0xff, 0xd9);

} // namespace barCfg
