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

/**
 * Locate the icon for an app_id. Tries the plain name first (see find()),
 * then falls back to the .desktop files: an app_id often matches the file's
 * basename / StartupWMClass / Name (e.g. app_id "qtcreator" with the icon
 * stored as QtProject-qtcreator.png) instead of the icon name itself.
 */
QString findForAppId(const QString &appId);
}
