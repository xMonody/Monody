#include "TrayWatcher.h"

#include "NotificationDaemon.h"
#include "TrayItem.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QtDebug>

TrayWatcher::TrayWatcher(QObject *parent)
    : QObject(parent)
{
    // Watch for apps quitting: remove their tray item when the owner of the
    // item's service name disappears.
    m_nameWatcher = new QDBusServiceWatcher(this);
    m_nameWatcher->setConnection(QDBusConnection::sessionBus());
    m_nameWatcher->setWatchMode(QDBusServiceWatcher::WatchForOwnerChange);
    connect(m_nameWatcher, &QDBusServiceWatcher::serviceOwnerChanged,
            this, &TrayWatcher::onNameOwnerChanged);
}

bool TrayWatcher::registerService()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerService(QStringLiteral("org.kde.StatusNotifierWatcher"))) {
        qWarning() << "tray: cannot register StatusNotifierWatcher:"
                   << bus.lastError().message();
        return false;
    }    // Expose the interface on both object paths: the spec's
    // /StatusNotifierWatcher and the /org/kde/... spelling used by some
    // clients.
    const auto flags = QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties;
    if (!bus.registerObject(QStringLiteral("/StatusNotifierWatcher"), this, flags)
        || !bus.registerObject(QStringLiteral("/org/kde/StatusNotifierWatcher"), this, flags)) {
        qWarning() << "tray: cannot register /StatusNotifierWatcher:"
                   << bus.lastError().message();
        return false;
    }
    // We are the host as well as the watcher: claim the host role so
    // clients see IsStatusNotifierHostRegistered=true right away.
    RegisterStatusNotifierHost(bus.baseService());
    return true;
}

void TrayWatcher::connectNotificationDaemon(NotificationDaemon *daemon)
{
    connect(daemon, &NotificationDaemon::notificationReceived,
            this, &TrayWatcher::onNotificationReceived);
}

void TrayWatcher::onNotificationReceived(quint64 pid)
{
    if (pid == 0)
        return;
    for (TrayItem *item : std::as_const(m_items)) {
        if (item->pid() == pid) {
            item->flashAttention();
            break;
        }
    }
}

void TrayWatcher::RegisterStatusNotifierHost(const QString &service)
{
    if (!m_hostRegistered) {
        m_hostRegistered = true;
        m_hostService = service;
        sendHostRegistered();
    }
}

QStringList TrayWatcher::GetRegisteredItems()
{
    return m_registeredList;
}

void TrayWatcher::RegisterStatusNotifierItem(const QString &service)
{
    // The spec allows two forms: a well-known name like
    // "org.kde.StatusNotifierItem-123-1" (object path /StatusNotifierItem),
    // or an object path like "/org/ayatana/NotificationItem/foo" (the
    // caller's connection name is then the service).
    if (service.startsWith(QLatin1Char('/')))
        addItem(message().service(), service);
    else
        addItem(service, QStringLiteral("/StatusNotifierItem"));
    m_registeredList.append(service);
    sendItemRegistered(service);
}

void TrayWatcher::addItem(const QString &service, const QString &path)
{
    const QString key = service + QLatin1Char('|') + path;
    if (m_indexByKey.contains(key))
        return; // already known

    auto *item = new TrayItem(service, path, this);
    m_indexByKey.insert(key, m_items.size());
    m_serviceByKey.insert(key, service);
    m_items.append(item);
    m_nameWatcher->addWatchedService(service);
    emit itemAdded(item);
}

void TrayWatcher::onNameOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner)
{
    if (!newOwner.isEmpty())
        return; // a new owner appeared; nothing to remove
    Q_UNUSED(oldOwner)

    if (name == m_hostService) {
        // our registered host vanished; if it was not us, drop the role
        m_hostRegistered = false;
        m_hostService.clear();
        sendHostUnregistered();
        return;
    }

    const QString key = m_serviceByKey.key(name);
    if (key.isEmpty())
        return; // not a tray item we track

    TrayItem *item = nullptr;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i)->key() == key) {
            item = m_items.takeAt(i);
            break;
        }
    }
    if (!item)
        return;

    m_indexByKey.remove(key);
    m_serviceByKey.remove(key);
    m_registeredList.removeOne(name);
    m_nameWatcher->removeWatchedService(name);

    emit itemRemoved(item);
    sendItemUnregistered(name);
    item->deleteLater();
}

// ---------------------------------------------------------------------------
// DBus signals (sent manually so internal TrayItem* signals stay internal)
// ---------------------------------------------------------------------------

void TrayWatcher::sendItemRegistered(const QString &service)
{
    QDBusMessage sig = QDBusMessage::createSignal(
        QStringLiteral("/StatusNotifierWatcher"),
        QStringLiteral("org.kde.StatusNotifierWatcher"),
        QStringLiteral("StatusNotifierItemRegistered"));
    sig << service;
    QDBusConnection::sessionBus().send(sig);
}

void TrayWatcher::sendItemUnregistered(const QString &service)
{
    QDBusMessage sig = QDBusMessage::createSignal(
        QStringLiteral("/StatusNotifierWatcher"),
        QStringLiteral("org.kde.StatusNotifierWatcher"),
        QStringLiteral("StatusNotifierItemUnregistered"));
    sig << service;
    QDBusConnection::sessionBus().send(sig);
}

void TrayWatcher::sendHostRegistered()
{
    QDBusMessage sig = QDBusMessage::createSignal(
        QStringLiteral("/StatusNotifierWatcher"),
        QStringLiteral("org.kde.StatusNotifierWatcher"),
        QStringLiteral("StatusNotifierHostRegistered"));
    QDBusConnection::sessionBus().send(sig);
}

void TrayWatcher::sendHostUnregistered()
{
    QDBusMessage sig = QDBusMessage::createSignal(
        QStringLiteral("/StatusNotifierWatcher"),
        QStringLiteral("org.kde.StatusNotifierWatcher"),
        QStringLiteral("StatusNotifierHostUnregistered"));
    QDBusConnection::sessionBus().send(sig);
}
