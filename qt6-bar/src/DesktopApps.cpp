#include "DesktopApps.h"

#include "IconFinder.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

namespace {

bool isTrue(const QSettings &s, const QString &key)
{
    const QString v = s.value(key).toString().trimmed().toLower();
    return v == QLatin1String("true") || v == QLatin1String("yes") || v == QLatin1String("1");
}

// Localized value: prefer Chinese (zh_CN / zh) whenever present, then the
// system locale, then the plain key. Keys need the [Desktop Entry] group.
QString localizedValue(QSettings &s, const QString &key)
{
    const QLocale loc = QLocale::system();
    const QStringList keys = {
        key + QStringLiteral("[zh_CN]"),
        key + QStringLiteral("[zh]"),
        key + QLatin1Char('[') + loc.name() + QLatin1Char(']'),
        key + QLatin1Char('[') + loc.name().section(QLatin1Char('_'), 0, 0) + QLatin1Char(']'),
        key,
    };
    for (const QString &k : keys) {
        const QString v = s.value(QStringLiteral("Desktop Entry/") + k).toString().trimmed();
        if (!v.isEmpty())
            return v;
    }
    return {};
}

// Icon= may be a bare theme name, a name with an extension, or an absolute path.
// Only strip a REAL image extension: dotted ids like org.wezfurlong.wezterm must pass through.
QString resolveIcon(const QString &icon)
{
    if (icon.isEmpty())
        return {};
    const QString trimmed = icon.trimmed();
    if (trimmed.startsWith(QLatin1Char('/')))
        return QUrl::fromLocalFile(trimmed).toString();
    QString name = trimmed;
    static const QStringList imageExts = {QStringLiteral("png"), QStringLiteral("svg"),
                                          QStringLiteral("svgz"), QStringLiteral("xpm")};
    if (imageExts.contains(QFileInfo(name).suffix().toLower()))
        name = QFileInfo(name).completeBaseName();
    return IconFinder::find(name);
}

} // namespace

DesktopAppsModel::DesktopAppsModel(QObject *parent)
    : QAbstractListModel(parent)
{
    reload();
}

int DesktopAppsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QVariant DesktopAppsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const DesktopApp &app = m_items.at(index.row());
    switch (role) {
    case NameRole:
        return app.name;
    case IconRole:
        return app.icon;
    case ExecRole:
        return app.exec;
    default:
        return {};
    }
}

QHash<int, QByteArray> DesktopAppsModel::roleNames() const
{
    return {
        {NameRole, "name"},
        {IconRole, "icon"},
        {ExecRole, "exec"},
    };
}

void DesktopAppsModel::reload()
{
    // User dirs first so identical file names override the system ones.
    QStringList dirs;
    const QByteArray xdgData = qgetenv("XDG_DATA_HOME");
    const QString userData = xdgData.isEmpty() ? QString(QDir::homePath() + QLatin1String("/.local/share"))
                                               : QString::fromLocal8Bit(xdgData);
    dirs << userData + QLatin1String("/applications")
         << QStringLiteral("/usr/local/share/applications")
         << QStringLiteral("/usr/share/applications");

    QList<DesktopApp> apps;
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
            const QString type = s.value(QStringLiteral("Desktop Entry/Type")).toString().trimmed();
            if (type != QLatin1String("Application"))
                continue;
            if (isTrue(s, QStringLiteral("Desktop Entry/NoDisplay"))
                || isTrue(s, QStringLiteral("Desktop Entry/Hidden")))
                continue;
            const QString tryExec = s.value(QStringLiteral("Desktop Entry/TryExec")).toString().trimmed();
            if (!tryExec.isEmpty() && QStandardPaths::findExecutable(tryExec).isEmpty())
                continue;
            const QString exec = s.value(QStringLiteral("Desktop Entry/Exec")).toString().trimmed();
            if (exec.isEmpty())
                continue;

            DesktopApp app;
            app.name = localizedValue(s, QStringLiteral("Name"));
            if (app.name.isEmpty())
                app.name = QFileInfo(file).completeBaseName();
            app.exec = exec;
            app.icon = resolveIcon(s.value(QStringLiteral("Desktop Entry/Icon")).toString());
            apps.append(app);
        }
    }

    std::sort(apps.begin(), apps.end(), [](const DesktopApp &a, const DesktopApp &b) {
        return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });

    beginResetModel();
    m_items = apps;
    endResetModel();
}
