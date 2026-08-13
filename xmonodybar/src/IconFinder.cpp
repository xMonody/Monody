#include "IconFinder.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSet>
#include <QUrl>

namespace {

// One parsed .desktop file, used only to map an app_id to its icon name.
struct DesktopEntry
{
    QString fileId;  // file basename without .desktop
    QString wmClass; // StartupWMClass=
    QString name;    // Name=
    QString icon;    // Icon= (raw value)
};

QStringList applicationDirs()
{
    const QByteArray xdgData = qgetenv("XDG_DATA_HOME");
    const QString userData = xdgData.isEmpty() ? QString(QDir::homePath() + QLatin1String("/.local/share"))
                                               : QString::fromLocal8Bit(xdgData);
    return {userData + QLatin1String("/applications"),
            QStringLiteral("/usr/local/share/applications"),
            QStringLiteral("/usr/share/applications")};
}

/** Parse the .desktop files once and cache the result. */
const QVector<DesktopEntry> &desktopEntries()
{
    static const QVector<DesktopEntry> entries = [] {
        QVector<DesktopEntry> out;
        QSet<QString> seen;
        for (const QString &dirPath : applicationDirs()) {
            const QDir dir(dirPath);
            if (!dir.exists())
                continue;
            const QStringList files = dir.entryList(QStringList{QStringLiteral("*.desktop")},
                                                    QDir::Files | QDir::Readable, QDir::Name);
            for (const QString &file : files) {
                if (seen.contains(file))
                    continue; // user entries override system ones
                seen.insert(file);

                QSettings s(dir.filePath(file), QSettings::IniFormat);
                if (s.value(QStringLiteral("Desktop Entry/Type")).toString().trimmed()
                    != QLatin1String("Application"))
                    continue;
                DesktopEntry e;
                e.fileId = QFileInfo(file).completeBaseName();
                e.wmClass = s.value(QStringLiteral("Desktop Entry/StartupWMClass")).toString().trimmed();
                e.name = s.value(QStringLiteral("Desktop Entry/Name")).toString().trimmed();
                e.icon = s.value(QStringLiteral("Desktop Entry/Icon")).toString().trimmed();
                if (!e.icon.isEmpty())
                    out.append(e);
            }
        }
        return out;
    }();
    return entries;
}

QString checkFile(const QString &base)
{
    static const char *const extensions[] = {"png", "svg", "svgz", "xpm"};
    for (const char *ext : extensions) {
        const QFileInfo fi(base + QLatin1Char('.') + QLatin1String(ext));
        if (fi.isFile())
            return QUrl::fromLocalFile(fi.absoluteFilePath()).toString();
    }
    return {};
}

QString findIconInThemeDirs(const QString &candidate)
{
    // Preferred icon sizes, largest first (Qt will downscale). 512 is in
    // the list because some apps (e.g. QQ) ship their icon only at 512x512.
    static const char *const sizes[] = {"512", "256", "128", "64", "48", "32", "24", "22", "16"};

    QStringList themeRoots;
    const QByteArray xdgData = qgetenv("XDG_DATA_HOME");
    const QString userData = xdgData.isEmpty() ? QString(QDir::homePath() + QLatin1String("/.local/share"))
                                               : QString::fromLocal8Bit(xdgData);
    themeRoots << userData + QLatin1String("/icons") << QStringLiteral("/usr/share/icons");

    for (const QString &root : themeRoots) {
        QDir base(root);
        if (!base.exists())
            continue;

        // hicolor: look through the per-size directories first.
        for (const char *size : sizes) {
            const QString sub = QStringLiteral("hicolor/%1x%1/apps/%2").arg(QLatin1String(size), candidate);
            if (const QString found = checkFile(base.filePath(sub)); !found.isEmpty())
                return found;
        }
        if (const QString found = checkFile(base.filePath(QStringLiteral("hicolor/scalable/apps/%1").arg(candidate))); !found.isEmpty())
            return found;

        // Any other installed theme.
        const QStringList themes = base.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &theme : themes) {
            if (theme == QLatin1String("hicolor"))
                continue;
            for (const char *size : sizes) {
                const QString sub = QStringLiteral("%1/%2x%2/apps/%3").arg(theme, QLatin1String(size), candidate);
                if (const QString found = checkFile(base.filePath(sub)); !found.isEmpty())
                    return found;
            }
            if (const QString found = checkFile(base.filePath(QStringLiteral("%1/scalable/apps/%2").arg(theme, candidate))); !found.isEmpty())
                return found;
        }
    }
    return {};
}

} // namespace

namespace IconFinder {

QString find(const QString &name)
{
    if (name.isEmpty())
        return {};

    // Absolute path (Icon= in a .desktop file is often an absolute path,
    // e.g. /usr/share/icons/hicolor/512x512/apps/qq.png): use it as-is.
    if (name.startsWith(QLatin1Char('/'))) {
        const QFileInfo fi(name);
        if (fi.isFile())
            return QUrl::fromLocalFile(fi.absoluteFilePath()).toString();
        return {};
    }

    // Try a few common spellings of the icon/app_id.
    QList<QString> candidates;
    auto push = [&candidates](const QString &c) {
        if (!c.isEmpty() && !candidates.contains(c))
            candidates.append(c);
    };
    push(name);
    push(name.toLower());
    QString dashless = name;
    push(dashless.replace(QLatin1Char('-'), QLatin1Char('_')));
    QString dashlessLower = name.toLower();
    push(dashlessLower.replace(QLatin1Char('-'), QLatin1Char('_')));

    for (const QString &candidate : std::as_const(candidates)) {
        if (const QString found = findIconInThemeDirs(candidate); !found.isEmpty())
            return found;

        // Flat pixmap directories.
        if (const QString found = checkFile(QStringLiteral("/usr/share/pixmaps/") + candidate); !found.isEmpty())
            return found;
        const QByteArray xdgData = qgetenv("XDG_DATA_HOME");
        const QString userData = xdgData.isEmpty() ? QString(QDir::homePath() + QLatin1String("/.local/share"))
                                                   : QString::fromLocal8Bit(xdgData);
        if (const QString found = checkFile(userData + QLatin1String("/pixmaps/") + candidate); !found.isEmpty())
            return found;
    }
    return {};
}

QString findForAppId(const QString &appId)
{
    if (appId.isEmpty())
        return {};

    if (const QString direct = find(appId); !direct.isEmpty())
        return direct;

    // The app_id may not match the icon file name at all: e.g. Qt Creator
    // reports "qtcreator" / "org.qt-project.qtcreator" while its icon is
    // stored as QtProject-qtcreator.png. Match the app_id against the
    // .desktop files (basename, StartupWMClass, Name) and retry with the
    // Icon= value they carry.
    const QString lower = appId.toLower();
    for (const DesktopEntry &e : desktopEntries()) {
        const bool byFileId = e.fileId == appId || e.fileId.toLower() == lower;
        const bool byWmClass = !e.wmClass.isEmpty()
                               && (e.wmClass == appId || e.wmClass.toLower() == lower);
        const bool byName = !e.name.isEmpty()
                            && (e.name.compare(appId, Qt::CaseInsensitive) == 0);
        if (byFileId || byWmClass || byName) {
            if (const QString found = find(e.icon); !found.isEmpty())
                return found;
        }
    }
    return {};
}

} // namespace IconFinder
