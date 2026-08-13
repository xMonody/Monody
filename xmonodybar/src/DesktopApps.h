#pragma once

#include <QAbstractListModel>
#include <QList>

struct DesktopApp
{
    QString name;   // localized display name
    QString icon;   // resolved icon URL (file://...) or empty
    QString exec;   // raw Exec line from the .desktop file
};

/**
 * Flat list of installed applications, read from the XDG application
 * directories (~/.local/share/applications, /usr/local/share/applications,
 * /usr/share/applications). User entries override system ones with the same
 * file name; NoDisplay/Hidden/Type!=Application entries are skipped.
 */
class DesktopAppsModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        IconRole,
        ExecRole,
    };

    explicit DesktopAppsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /** Rescan the XDG application directories. */
    Q_INVOKABLE void reload();

private:
    QList<DesktopApp> m_items;
};
