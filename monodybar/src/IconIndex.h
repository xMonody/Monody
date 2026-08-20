#pragma once

#include <QHash>
#include <QSet>
#include <QString>

/**
 * Icon index built once when the status bar starts.
 *
 * The XDG .desktop files and the icon themes are scanned a single time and
 * the results are kept in memory, so drawing an icon is a plain hash lookup
 * and never touches the filesystem again:
 *
 *   exec first token       -> icon path   (Icon= resolution, Exec=firefox %u)
 *   app_id                 -> icon path   (desktop file id / StartupWMClass / Name)
 *   themed icon name       -> icon path   (tray icons, attention icons, ...)
 *
 * Icon= values are resolved against the icon themes in this order:
 *   $XDG_DATA_HOME/icons (default ~/.local/share/icons) first,
 *   then /usr/share/icons, then /usr/local/share/icons;
 *   the flat pixmap dirs are a last resort.
 * Every stored value is an absolute file path.
 *
 * Real icon files are indexed before symlink aliases (fcitx.svg ->
 * org.fcitx.Fcitx5.svg), and the user theme's definitions win over the
 * system themes' defaults: WhiteSur's links/apps/scalable/nvim.svg ->
 * neovim.svg makes "nvim" resolve to WhiteSur's neovim.svg, foot.svg ->
 * terminal.svg makes "foot" resolve to the terminal SVG, etc. The themed
 * icon name is preferred over the .desktop Icon= mapping.
 */
class IconIndex
{
public:
    /** Scan the .desktop files and icon themes once. Idempotent. */
    static void init();

    /** Icon for the first token of a .desktop Exec= line, or empty. */
    static QString iconForExec(const QString &exec);

    /** Icon for a window app_id (desktop id / StartupWMClass / Name, then the themed name). */
    static QString iconForAppId(const QString &appId);

    /** Icon for a bare themed icon name (Icon= values, tray names), or empty. */
    static QString iconForName(const QString &name);

private:
    static void scanThemeDirs();
    static void scanDesktopFiles();
    static void indexDirRecursive(const QString &dirPath, QHash<QString, QString> &out,
                                  bool symlinksOnly);
    /** Resolve broken-link aliases (X -> target-name) into the name index. */
    static void resolveAliases();
    /** Theme-only chain resolution of an alias target, with a cycle guard. */
    static QString resolveAliasChain(const QString &target, QSet<QString> &seen);
    /** Recursive lookup: themed name first, then the desktop file mapping. */
    static QString lookupRecursive(const QString &lowerKey, QSet<QString> &seen);

    // Exec first token (lowercased) -> icon path
    static QHash<QString, QString> s_execToIcon;
    // Desktop file id / StartupWMClass / Name (lowercased) -> icon path
    static QHash<QString, QString> s_appToIcon;
    // Themed icon name (lowercased) -> icon path
    static QHash<QString, QString> s_nameToIcon;
    // Broken symlink alias: name (lowercased) -> target icon name
    static QHash<QString, QString> s_aliasToIcon;
};
