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
        // QML re-request the image when the item changes its icon.  While
        // the item wants attention (message received) the attention icon is
        // used instead - same provider, same key, the model resolves it.
        const int rev = m_revisions.value(item, 0);
        const auto enc = [](const QString &s) {
            return QString::fromLatin1(QUrl::toPercentEncoding(s));
        };
        if (item->attentionActive() && item->hasAttentionPixmap())
            return QStringLiteral("image://trayicons/%1?v=%2")
                .arg(enc(item->key()), QString::number(rev));
        if (item->attentionActive() && !item->attentionIconName().isEmpty())
            return QStringLiteral("image://icons/%1")
                .arg(enc(item->attentionIconName()));
        if (item->hasPixmap())
            return QStringLiteral("image://trayicons/%1?v=%2")
                .arg(enc(item->key()), QString::number(rev));
        if (!item->iconName().isEmpty())
            return QStringLiteral("image://icons/%1")
                .arg(enc(item->iconName()));
        return {};
    }
    case AttentionRole:
        return item->attentionActive();
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
        { AttentionRole, "attention" },
    };
}

QImage TrayModel::pixmap(const QString &key) const
{
    for (TrayItem *item : m_items) {
        if (item->key() == key) {
            if (item->attentionActive() && item->hasAttentionPixmap())
                return item->attentionIconPixmap();
            return item->iconPixmap();
        }
    }
    return {};
}

void TrayModel::activate(int row)
{
    if (row >= 0 && row < m_items.size())
        m_items.at(row)->activate(0, 0);
}

void TrayModel::secondaryActivate(int row)
{
    if (row >= 0 && row < m_items.size())
        m_items.at(row)->secondaryActivate(0, 0);
}

bool TrayModel::fetchMenu(int row)
{
    if (row < 0 || row >= m_items.size())
        return false;
    return m_items.at(row)->fetchMenu();
}

QVariantList TrayModel::menuItems(int row, int parentId)
{
    if (row < 0 || row >= m_items.size())
        return {};
    const QList<MenuItem> children = m_items.at(row)->menuChildren(parentId);
    QVariantList out;
    out.reserve(children.size());
    for (const MenuItem &mi : children) {
        if (!mi.visible)
            continue;
        out << QVariantMap{
            { QStringLiteral("id"), mi.id },
            { QStringLiteral("label"), mi.label },
            { QStringLiteral("enabled"), mi.enabled },
            { QStringLiteral("type"), mi.type },
            { QStringLiteral("hasChildren"), !mi.children.isEmpty() },
        };
    }
    return out;
}

bool TrayModel::triggerMenu(int row, int id)
{
    if (row < 0 || row >= m_items.size())
        return false;
    return m_items.at(row)->triggerMenuItem(id);
}

void TrayModel::onItemAdded(TrayItem *item)
{
    // Keep input-method items (fcitx5) at the very end of the tray, right
    // next to the clock; every other app stays in front of them, in arrival
    // order.  The process name comes from /proc/<pid>/comm (see TrayItem).
    int insertAt = m_items.size();
    if (!item->processName().startsWith(QLatin1String("fcitx"), Qt::CaseInsensitive)) {
        for (int i = 0; i < m_items.size(); ++i) {
            if (m_items.at(i)->processName().startsWith(QLatin1String("fcitx"), Qt::CaseInsensitive)) {
                insertAt = i;
                break;
            }
        }
    }
    beginInsertRows(QModelIndex(), insertAt, insertAt);
    m_items.insert(insertAt, item);
    endInsertRows();
    emit countChanged();
    connect(item, &TrayItem::iconChanged, this, &TrayModel::onItemChanged);
    connect(item, &TrayItem::titleChanged, this, &TrayModel::onItemChanged);
    connect(item, &TrayItem::attentionChanged, this, &TrayModel::onItemChanged);
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
        emit dataChanged(index(row), index(row), { IconRole, TitleRole, AttentionRole });
}
