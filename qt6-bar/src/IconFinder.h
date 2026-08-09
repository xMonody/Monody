#pragma once

#include <QString>

/**
 * Locate an icon by name following the XDG icon theme layout:
 *   $XDG_DATA_HOME/icons and /usr/share/icons (hicolor + any theme, per-size
 *   and scalable dirs), then the flat pixmap dirs.
 *
 * Returns a file:// URL, or an empty string when nothing was found.
 * Also tries common spelling variants (lowercase, '-' -> '_').
 */
namespace IconFinder {
QString find(const QString &name);
}
