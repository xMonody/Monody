#include "TrayModel.h"

#include "TrayItem.h"
#include "TrayWatcher.h"

#include <QUrl>

TrayModel::TrayModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void TrayModel::setWatcher(TrayWatcher *watcher)
{
    m_watcher = watcher;
    connect(watcher, &TrayWatcher::itemAdded, this, &TrayModel::onItemAdded);
    connect(watcher, &TrayWatcher::itemRemoved, this, &TrayModel::onItemRemoved);
    for (int i = 0; i < watcher->itemCount(); ++i)
        onItemAdded(watcher->itemAt(i));
}

int TrayModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QVariant TrayModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_items.size())
        return {};
    TrayItem *item = m_items.at(index.row());

    switch (role) {
    case IconRole: {
        // Prefer the item-provided bitmap; fall back to the themed icon by
        // name (reuses the bar's icon provider).  A revision suffix makes
        // QML re-request the image when the item changes its icon.
        const int rev = m_revisions.value(item, 0);
        if (item->hasPixmap())
            return QStringLiteral("image://trayicons/%1?v=%2")
                .arg(QString::fromLatin1(QUrl::toPercentEncoding(item->key())),
                     QString::number(rev));
        if (!item->iconName().isEmpty())
            return QStringLiteral("image://icons/%1")
                .arg(QString::fromLatin1(QUrl::toPercentEncoding(item->iconName())));
        return {};
    }
    case TitleRole:
        return item->title();
    case ItemKeyRole:
        return item->key();
    }
    return {};
}

QHash<int, QByteArray> TrayModel::roleNames() const
{
    return {
        { IconRole, "icon" },
        { TitleRole, "title" },
        { ItemKeyRole, "itemKey" },
    };
}

QImage TrayModel::pixmap(const QString &key) const
{
    for (TrayItem *item : m_items) {
        if (item->key() == key)
            return item->iconPixmap();
    }
    return {};
}

void TrayModel::activate(int row)
{
    if (row >= 0 && row < m_items.size())
        m_items.at(row)->activate(0, 0);
}

void TrayModel::contextMenu(int row)
{
    if (row >= 0 && row < m_items.size())
        m_items.at(row)->contextMenu(0, 0);
}

void TrayModel::secondaryActivate(int row)
{
    if (row >= 0 && row < m_items.size())
        m_items.at(row)->secondaryActivate(0, 0);
}

void TrayModel::onItemAdded(TrayItem *item)
{
    beginInsertRows(QModelIndex(), m_items.size(), m_items.size());
    m_items.append(item);
    endInsertRows();
    emit countChanged();
    connect(item, &TrayItem::iconChanged, this, &TrayModel::onItemChanged);
    connect(item, &TrayItem::titleChanged, this, &TrayModel::onItemChanged);
}

void TrayModel::onItemRemoved(TrayItem *item)
{
    const int row = m_items.indexOf(item);
    if (row < 0)
        return;
    beginRemoveRows(QModelIndex(), row, row);
    m_items.removeAt(row);
    endRemoveRows();
    emit countChanged();
    m_revisions.remove(item);
}

void TrayModel::onItemChanged()
{
    TrayItem *item = qobject_cast<TrayItem *>(sender());
    if (!item)
        return;
    m_revisions[item] = m_revisions.value(item, 0) + 1;
    const int row = m_items.indexOf(item);
    if (row >= 0)
        emit dataChanged(index(row), index(row), { IconRole, TitleRole });
}
