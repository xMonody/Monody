#pragma once

#include <QDBusVirtualObject>
#include <QObject>

/**
 * The org.freedesktop.Notifications service (freedesktop notification
 * daemon), exposed on /org/freedesktop/Notifications.
 *
 * QQ (and friends) do NOT signal a new message through StatusNotifierItem:
 * a message arrives only as a desktop notification (Notify call), which is
 * otherwise dropped because this desktop has no notification daemon.  By
 * owning the name ourselves we learn about every incoming message and can
 * blink the matching tray icon (matched by the notifying process's pid).
 *
 * Implemented as a QDBusVirtualObject on purpose: libnotify strictly
 * validates the GetServerInformation reply type with
 * g_variant_is_of_type("(ssss)"), and a Qt slot returning a struct is
 * marshalled as one (ssss) argument which GDBus delivers as ((ssss)) - a
 * "Unexpected reply type" failure.  Returning the four strings as four
 * separate reply arguments (like dunst/swaync do) yields exactly (ssss).
 *
 * Notifications are intentionally not shown as popups - they only trigger
 * the tray-icon flash.
 */
class NotificationDaemon : public QDBusVirtualObject
{
    Q_OBJECT
public:
    explicit NotificationDaemon(QObject *parent = nullptr);

    /** Claim "org.freedesktop.Notifications" on the session bus. */
    bool registerService();

signals:
    /** A notification was posted by the process with this pid. */
    void notificationReceived(quint64 pid);

protected:
    bool handleMessage(const QDBusMessage &message,
                       const QDBusConnection &connection) override;
    QString introspect(const QString &path) const override;

private:
    quint32 m_nextId = 1;
};
