#pragma once

#include <QDBusContext>
#include <QDBusServiceWatcher>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class TrayItem;
class NotificationDaemon;

/**
 * The org.kde.StatusNotifierWatcher DBus service (the system-tray host).
 *
 * Apps that want a tray icon (fcitx5, QQ, WeChat, ...) call
 * RegisterStatusNotifierItem on this service; each one becomes a TrayItem.
 * The item disappears automatically when the app quits (NameOwnerChanged).
 *
 * The interface is exposed on both /StatusNotifierWatcher and
 * /org/kde/StatusNotifierWatcher so every client finds it regardless of
 * which object path it uses.
 */
class TrayWatcher : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierWatcher")
    Q_PROPERTY(QStringList RegisteredStatusNotifierItems READ registeredItems)
    Q_PROPERTY(bool IsStatusNotifierHostRegistered READ isHostRegistered)
    Q_PROPERTY(int ProtocolVersion READ protocolVersion)
public:
    explicit TrayWatcher(QObject *parent = nullptr);

    /** Claim "org.kde.StatusNotifierWatcher" on the session bus. */
    bool registerService();

    /**
     * Route incoming freedesktop notifications to the tray item owned by
     * the notifying process (matched by pid), so apps like QQ that never
     * signal SNI attention can still flash their tray icon on messages.
     */
    void connectNotificationDaemon(NotificationDaemon *daemon);

    QStringList registeredItems() const { return m_registeredList; }
    bool isHostRegistered() const { return m_hostRegistered; }
    int protocolVersion() const { return 0; }

    TrayItem *itemAt(int index) const { return index >= 0 && index < m_items.size() ? m_items.at(index) : nullptr; }
    int itemCount() const { return m_items.size(); }

public slots:
    void RegisterStatusNotifierItem(const QString &service);
    void RegisterStatusNotifierHost(const QString &service);
    /** KDE-compatible alias for the RegisteredStatusNotifierItems property. */
    QStringList GetRegisteredItems();

signals:
    void itemAdded(TrayItem *item);
    void itemRemoved(TrayItem *item);

private slots:
    void onNotificationReceived(quint64 pid);

private:
    void sendItemRegistered(const QString &service);
    void sendItemUnregistered(const QString &service);
    void sendHostRegistered();
    void sendHostUnregistered();
    void onNameOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner);
    void addItem(const QString &service, const QString &path);

    QDBusServiceWatcher *m_nameWatcher = nullptr;
    QList<TrayItem *> m_items;         // insertion order
    QHash<QString, int> m_indexByKey;  // item key -> index in m_items
    QHash<QString, QString> m_serviceByKey; // item key -> item service name
    QStringList m_registeredList;      // values passed to RegisterStatusNotifierItem
    bool m_hostRegistered = false;
    QString m_hostService;
};
