#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QList>

class TrayItem;
class TrayWatcher;

/**
 * QAbstractListModel over the tray items, exposed to QML (context property
 * "trayModel").  Each row is one StatusNotifierItem with roles:
 *   icon     - image provider URL (re-requested when the icon changes)
 *   title    - item Title (tooltip text)
 *   itemKey  - unique key
 */
class TrayModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum Roles {
        IconRole = Qt::UserRole + 1,
        TitleRole,
        ItemKeyRole,
    };

    explicit TrayModel(QObject *parent = nullptr);
    void setWatcher(TrayWatcher *watcher);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /** Icon bitmap for the item with the given key (TrayIconProvider). */
    QImage pixmap(const QString &key) const;

    /** User actions, called from QML. */
    Q_INVOKABLE void activate(int row);
    Q_INVOKABLE void contextMenu(int row);
    Q_INVOKABLE void secondaryActivate(int row);

signals:
    void countChanged();

private slots:
    void onItemAdded(TrayItem *item);
    void onItemRemoved(TrayItem *item);
    void onItemChanged(); // icon/title of the sender() item changed

private:
    QList<TrayItem *> m_items;
    QHash<TrayItem *, int> m_revisions; // bumped on every icon change
    TrayWatcher *m_watcher = nullptr;
};
