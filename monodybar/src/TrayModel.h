#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QList>

class TrayItem;
class TrayWatcher;

/**
 * QAbstractListModel over the tray items, exposed to QML (context property
 * "trayModel").  Each row is one StatusNotifierItem with roles:
 *   icon      - image provider URL (re-requested when the icon changes)
 *   title     - item Title (tooltip text)
 *   itemKey   - unique key
 *   attention - true while the item wants attention (blink the icon)
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
        AttentionRole,
    };

    explicit TrayModel(QObject *parent = nullptr);
    void setWatcher(TrayWatcher *watcher);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /** Icon bitmap for the item with the given key (TrayIconProvider). */
    QImage pixmap(const QString &key) const;

    /** User actions, called from QML. x/y are screen coordinates for the
     *  item's own context menu (see SNI ContextMenu). */
    Q_INVOKABLE void activate(int row);
    Q_INVOKABLE void secondaryActivate(int row);

    // -- app context menu (com.canonical.dbusmenu via the SNI Menu property) --
    /** Load the item's menu tree; true when the app provides one. */
    Q_INVOKABLE bool fetchMenu(int row);
    /** Menu entries under parentId (0 = top level), as QVariantMap list. */
    Q_INVOKABLE QVariantList menuItems(int row, int parentId);
    /** Trigger a "clicked" event on a menu item; returns true on success. */
    Q_INVOKABLE bool triggerMenu(int row, int id);

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
