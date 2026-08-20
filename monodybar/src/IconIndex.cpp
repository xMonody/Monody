#include "IconIndex.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSet>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace {

// Real image extensions, case-insensitive. Dotted ids like
// org.wezfurlong.wezterm keep all their dots.
bool isImageExt(const QString &fileName)
{
    const QString ext = QFileInfo(fileName).suffix().toLower();
    return ext == QLatin1String("png") || ext == QLatin1String("svg")
           || ext == QLatin1String("svgz") || ext == QLatin1String("xpm");
}

/** Insert only when the key is not taken, so the first (higher-priority) hit wins. */
void insertUnique(QHash<QString, QString> &hash, const QString &key, const QString &path)
{
    if (!key.isEmpty() && !hash.contains(key))
        hash.insert(key, path);
}

/**
 * Recursively index every icon file under dirPath by its complete base name
 * (QtProject-qtcreator.png -> "qtproject-qtcreator"). Deterministic: entries
 * are walked in sorted order.
 *
 * symlinksOnly selects which pass this is: real files first, then symlink
 * aliases (fcitx.svg -> org.fcitx.Fcitx5.svg) for names that are still
 * missing. A symlink can therefore never shadow a real icon - a links/
 * "container" like WhiteSur's (foot.svg -> terminal.svg) cannot replace the
 * real foot logo. Broken symlinks (invisible to QDir, so listed via
 * std::filesystem) are stored as name aliases instead, e.g.
 * org.codeberg.dnkl.foot.svg -> foot.svg makes the app_id
 * "org.codeberg.dnkl.foot" resolve to the foot icon. Symlinked directories
 * are not followed (loop safety).
 */
} // namespace

void IconIndex::indexDirRecursive(const QString &dirPath, QHash<QString, QString> &out,
                                  bool symlinksOnly)
{
    std::error_code ec;
    std::filesystem::directory_iterator it(dirPath.toStdString(), ec);
    if (ec)
        return;

    // Collect the entry names first (sorted) so the walk is deterministic.
    QStringList names;
    for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec)
            break;
        names << QString::fromStdString(it->path().filename().string());
    }
    names.sort();

    for (const QString &entry : std::as_const(names)) {
        const QString full = dirPath + QLatin1Char('/') + entry;
        const QFileInfo fi(full);
        if (fi.isDir()) {
            if (!fi.isSymLink())
                indexDirRecursive(full, out, symlinksOnly);
            continue;
        }
        if (!isImageExt(entry))
            continue;
        const QString name = fi.completeBaseName().toLower();
        if (name.isEmpty())
            continue;

        if (symlinksOnly) {
            if (!fi.isSymLink())
                continue;
            if (fi.exists()) {
                // Valid alias: index the symlink itself (renders its target).
                insertUnique(out, name, full);
            } else {
                // Broken alias: remember name -> target icon name, e.g.
                // org.vim.Vim.svg -> vim.svg  =>  "org.vim.vim" -> "vim".
                const QString target =
                    QFileInfo(fi.symLinkTarget()).completeBaseName().toLower();
                if (!target.isEmpty() && target != name)
                    insertUnique(s_aliasToIcon, name, target);
            }
        } else if (!fi.isSymLink()) {
            insertUnique(out, name, full); // real files first
        }
    }
}

namespace {

/** First whitespace-separated token of an Exec= line (quotes stripped). */
QString execFirstToken(const QString &exec)
{
    QString first = exec.section(QChar(' '), 0, 0).trimmed();
    if (first.size() >= 2 && first.startsWith(QLatin1Char('"'))
        && first.endsWith(QLatin1Char('"')))
        first = first.mid(1, first.size() - 2);
    return first;
}

QString userDataDir()
{
    const QByteArray xdgData = qgetenv("XDG_DATA_HOME");
    if (!xdgData.isEmpty())
        return QString::fromLocal8Bit(xdgData);
    return QDir::homePath() + QLatin1String("/.local/share");
}

/**
 * Resolve an Icon= value to an absolute path. Only called while building the
 * index: themed-name lookup first, flat pixmap dirs as a last resort.
 */
QString resolveIconValue(const QString &icon, const QString &userData)
{
    const QString trimmed = icon.trimmed();
    if (trimmed.isEmpty())
        return {};
    if (const QString found = IconIndex::iconForName(trimmed); !found.isEmpty())
        return found;

    // The name was not in any theme: try the flat pixmap dirs.
    QString name = trimmed;
    if (name.startsWith(QLatin1Char('/')))
        return {}; // absolute path that does not exist
    if (isImageExt(name))
        name = QFileInfo(name).completeBaseName();
    if (name.isEmpty())
        return {};
    static const char *const exts[] = {"png", "svg", "svgz", "xpm"};
    for (const QString &root : {userData + QLatin1String("/pixmaps"),
                                QStringLiteral("/usr/share/pixmaps")}) {
        for (const char *ext : exts) {
            const QFileInfo fi(root + QLatin1Char('/') + name + QLatin1Char('.')
                               + QLatin1String(ext));
            if (fi.isFile())
                return fi.absoluteFilePath();
        }
    }
    return {};
}

} // namespace

QHash<QString, QString> IconIndex::s_execToIcon;
QHash<QString, QString> IconIndex::s_appToIcon;
QHash<QString, QString> IconIndex::s_nameToIcon;
QHash<QString, QString> IconIndex::s_aliasToIcon;

void IconIndex::init()
{
    static bool built = false;
    if (built)
        return;
    built = true;
    scanThemeDirs();
    scanDesktopFiles();
}

void IconIndex::scanThemeDirs()
{
    // Priority: the user's own icons first, then the system themes.
    const QString userData = userDataDir();
    const QStringList roots = {
        userData + QLatin1String("/icons"),
        QStringLiteral("/usr/share/icons"),
        QStringLiteral("/usr/local/share/icons"),
    };

    // Walk one theme (hicolor first, larger sizes first, then
    // scalable/symbolic dirs), indexing real files or symlink aliases.
    auto walkTheme = [](const QDir &themeDir, bool symlinksOnly) {
        QStringList sizeDirs, topDirs;
        const QStringList sub =
            themeDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &d : sub) {
            // "<size>x<size>" dirs are walked largest first (a bigger source
            // keeps more detail once Qt downscales); the rest (scalable,
            // symbolic, ...) get a plain recursive walk.
            if (d.contains(QLatin1Char('x')) && d.section(QChar('x'), 0, 0).toInt() > 0)
                sizeDirs << d;
            else
                topDirs << d;
        }
        std::sort(sizeDirs.begin(), sizeDirs.end(), [](const QString &a, const QString &b) {
            const int av = a.section(QChar('x'), 0, 0).toInt();
            const int bv = b.section(QChar('x'), 0, 0).toInt();
            return av != bv ? av > bv : a < b;
        });
        for (const QString &size : std::as_const(sizeDirs))
            indexDirRecursive(themeDir.filePath(size), s_nameToIcon, symlinksOnly);
        for (const QString &top : std::as_const(topDirs))
            indexDirRecursive(themeDir.filePath(top), s_nameToIcon, symlinksOnly);
    };

    // Each root is processed completely (real files, then symlink aliases,
    // then broken-link aliases resolved to their target icons) before the
    // next root, so everything the user theme defines - its real SVG icons
    // AND its links (nvim.svg -> neovim.svg, foot.svg -> terminal.svg, ...) -
    // wins over the system themes' defaults.
    for (const QString &root : std::as_const(roots)) {
        const QDir base(root);
        if (!base.exists())
            continue;

        // hicolor first (the fallback theme), then the others by name.
        QStringList themes =
            base.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        std::sort(themes.begin(), themes.end(), [](const QString &a, const QString &b) {
            const bool aH = a.compare(QLatin1String("hicolor"), Qt::CaseInsensitive) == 0;
            const bool bH = b.compare(QLatin1String("hicolor"), Qt::CaseInsensitive) == 0;
            return aH != bH ? aH : a < b;
        });

        // Pass A: real icon files.
        for (const QString &theme : std::as_const(themes))
            walkTheme(QDir(base.filePath(theme)), /*symlinksOnly=*/false);
        // Pass B: symlink aliases - valid ones index their own path, broken
        // ones are registered as name -> target-icon-name aliases.
        for (const QString &theme : std::as_const(themes))
            walkTheme(QDir(base.filePath(theme)), /*symlinksOnly=*/true);
        // Pass C: resolve this root's broken-link aliases (nvim -> neovim ->
        // WhiteSur's neovim.svg) into the name index, before lower-priority
        // roots can fill those names with their own defaults.
        resolveAliases();
    }
}

void IconIndex::scanDesktopFiles()
{
    const QString userData = userDataDir();
    const QStringList dirs = {
        userData + QLatin1String("/applications"),
        QStringLiteral("/usr/local/share/applications"),
        QStringLiteral("/usr/share/applications"),
    };

    // Same file name in several dirs: the user's entry overrides the system's.
    QSet<QString> seen;
    for (const QString &dirPath : std::as_const(dirs)) {
        const QDir dir(dirPath);
        if (!dir.exists())
            continue;
        const QStringList files = dir.entryList(QStringList{QStringLiteral("*.desktop")},
                                                QDir::Files | QDir::Readable, QDir::Name);
        for (const QString &file : files) {
            if (seen.contains(file))
                continue;
            seen.insert(file);

            QSettings s(dir.filePath(file), QSettings::IniFormat);
            if (s.value(QStringLiteral("Desktop Entry/Type")).toString().trimmed()
                != QLatin1String("Application"))
                continue;

            const QString iconRaw =
                s.value(QStringLiteral("Desktop Entry/Icon")).toString().trimmed();
            const QString path = resolveIconValue(iconRaw, userData);
            if (path.isEmpty())
                continue;

            // Desktop file id (e.g. "qtcreator")
            insertUnique(s_appToIcon, QFileInfo(file).completeBaseName().toLower(), path);
            // StartupWMClass - the app_id the compositor usually reports
            insertUnique(s_appToIcon,
                         s.value(QStringLiteral("Desktop Entry/StartupWMClass"))
                             .toString().trimmed().toLower(),
                         path);
            // Name, matched case-insensitively like the old .desktop matcher
            insertUnique(s_appToIcon,
                         s.value(QStringLiteral("Desktop Entry/Name"))
                             .toString().trimmed().toLower(),
                         path);
            // Exec first token (and its basename): the exec -> icon hash
            const QString first = execFirstToken(
                s.value(QStringLiteral("Desktop Entry/Exec")).toString().trimmed());
            if (!first.isEmpty()) {
                insertUnique(s_execToIcon, first.toLower(), path);
                const QString base = QFileInfo(first).fileName();
                if (!base.isEmpty() && base != first)
                    insertUnique(s_execToIcon, base.toLower(), path);
            }
        }
    }
}

QString IconIndex::iconForExec(const QString &exec)
{
    if (exec.isEmpty())
        return {};
    const QString first = execFirstToken(exec);
    if (first.isEmpty())
        return {};
    const auto it = s_execToIcon.constFind(first.toLower());
    if (it != s_execToIcon.constEnd())
        return it.value();
    const QString base = QFileInfo(first).fileName();
    if (!base.isEmpty() && base != first) {
        const auto it2 = s_execToIcon.constFind(base.toLower());
        if (it2 != s_execToIcon.constEnd())
            return it2.value();
    }
    return {};
}

QString IconIndex::iconForAppId(const QString &appId)
{
    if (appId.isEmpty())
        return {};
    QSet<QString> seen;
    return lookupRecursive(appId.toLower(), seen);
}

QString IconIndex::iconForName(const QString &name)
{
    if (name.isEmpty())
        return {};
    // Existing absolute path (Icon= in a .desktop file).
    if (name.startsWith(QLatin1Char('/'))) {
        const QFileInfo fi(name);
        return fi.isFile() ? fi.absoluteFilePath() : QString{};
    }
    QSet<QString> seen;
    return lookupRecursive(name.toLower(), seen);
}

QString IconIndex::lookupRecursive(const QString &lowerKey, QSet<QString> &seen)
{
    if (lowerKey.isEmpty() || seen.contains(lowerKey))
        return {};
    seen.insert(lowerKey);

    // 1) Themed icon by name: exact, without a real image extension
    //    ("firefox.png"), then '-' -> '_' ("input-keyboard-symbolic").
    //    Broken-link aliases were already resolved into s_nameToIcon at index
    //    time, so the user theme's definitions - its SVGs and its links -
    //    win over the system themes' defaults.
    QStringList candidates;
    auto push = [&candidates](const QString &c) {
        if (!c.isEmpty() && !candidates.contains(c))
            candidates.append(c);
    };
    push(lowerKey);
    if (isImageExt(lowerKey))
        push(QFileInfo(lowerKey).completeBaseName());
    QString dashless = candidates.last();
    dashless.replace(QLatin1Char('-'), QLatin1Char('_'));
    push(dashless);
    for (const QString &c : std::as_const(candidates)) {
        if (const auto it = s_nameToIcon.constFind(c); it != s_nameToIcon.constEnd())
            return it.value();
    }

    // 2) Desktop file mapping (file id / StartupWMClass / Name -> Icon=), as
    //    a fallback for app_ids without a themed icon (e.g. "code").
    if (const auto it = s_appToIcon.constFind(lowerKey); it != s_appToIcon.constEnd())
        return it.value();

    return {};
}

void IconIndex::resolveAliases()
{
    // Resolve every broken-link alias registered so far (X -> Y) to Y's icon
    // path, following chains, and store X in the name index under the usual
    // priority rules (first hit wins). Runs after each theme root, so the
    // user theme's links beat the system themes' direct icons: e.g.
    // nvim.svg -> neovim.svg makes "nvim" resolve to WhiteSur's neovim.svg,
    // and foot.svg -> terminal.svg makes "foot" resolve to the terminal SVG.
    for (auto it = s_aliasToIcon.constBegin(); it != s_aliasToIcon.constEnd(); ++it) {
        QSet<QString> seen;
        seen.insert(it.key());
        const QString path = resolveAliasChain(it.value(), seen);
        if (!path.isEmpty())
            insertUnique(s_nameToIcon, it.key(), path);
    }
}

QString IconIndex::resolveAliasChain(const QString &target, QSet<QString> &seen)
{
    // Theme-only resolution of the alias target: follow X -> Y -> Z chains
    // (with a cycle guard) until a real icon path shows up.
    QString cur = target.toLower();
    int hops = 0;
    while (hops++ < 16) {
        if (cur.isEmpty() || seen.contains(cur))
            return {};
        seen.insert(cur);
        if (const auto it = s_nameToIcon.constFind(cur); it != s_nameToIcon.constEnd())
            return it.value();
        const auto it = s_aliasToIcon.constFind(cur);
        if (it == s_aliasToIcon.constEnd())
            return {};
        cur = it.value();
    }
    return {};
}
