/*
 * Minimal stand-in for the generated export header of layer-shell-qt.
 * The real dev package is not installed; we link against the runtime
 * library (libLayerShellQtInterface.so.6) whose symbols are already
 * exported, so an empty visibility macro is sufficient on Linux.
 */
#pragma once

#define LAYERSHELLQT_EXPORT
