#include "IconFinder.h"

#include <QDir>
#include <QFileInfo>
#include <QUrl>

namespace {

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
    // Preferred icon sizes, largest first (Qt will downscale).
    static const char *const sizes[] = {"256", "128", "64", "48", "32", "24", "22", "16"};

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

} // namespace IconFinder
