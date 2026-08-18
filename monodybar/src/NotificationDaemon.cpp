#include "NotificationDaemon.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QFile>
#include <QtDebug>

NotificationDaemon::NotificationDaemon(QObject *parent)
    : QDBusVirtualObject(parent)
{
}

bool NotificationDaemon::registerService()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerService(QStringLiteral("org.freedesktop.Notifications"))) {
        qWarning() << "notifications: cannot register org.freedesktop.Notifications:"
                   << bus.lastError().message();
        return false;
    }
    if (!bus.registerVirtualObject(QStringLiteral("/org/freedesktop/Notifications"), this)) {
        qWarning() << "notifications: cannot register /org/freedesktop/Notifications:"
                   << bus.lastError().message();
        return false;
    }
    return true;
}

bool NotificationDaemon::handleMessage(const QDBusMessage &message,
                                       const QDBusConnection &connection)
{
    const QString member = message.member();

    if (member == QLatin1String("GetCapabilities")) {
        QDBusMessage reply = message.createReply();
        reply << QStringList{ QStringLiteral("actions"), QStringLiteral("body"),
                              QStringLiteral("icon-static") };
        connection.send(reply);
        return true;
    }

    if (member == QLatin1String("GetServerInformation")) {
        // Four separate reply strings (NOT a struct): libnotify checks the
        // reply with g_variant_is_of_type("(ssss)") and a struct reply would
        // come back as ((ssss)) from GDBus, failing that check.
        QDBusMessage reply = message.createReply();
        reply << QStringLiteral("monodybar") << QStringLiteral("monody")
              << QStringLiteral("1.0") << QStringLiteral("1.2");
        connection.send(reply);
        return true;
    }

    if (member == QLatin1String("Notify")) {
        // args: appName(s) replacesId(u) appIcon(s) summary(s) body(s)
        //       actions(as) hints(a{sv}) expireTimeout(i)
        const QVariantList args = message.arguments();

        // Identify the notifying process: prefer the sender's dbus unique
        // name (GetConnectionUnixProcessID), fall back to the sender-pid
        // hint (QQ provides it).  Matched against the tray items' pids.
        quint64 pid = 0;
        const QString sender = message.service();
        if (sender.startsWith(QLatin1Char(':'))) {
            const QDBusReply<uint> reply =
                connection.interface()->servicePid(sender);
            if (reply.isValid())
                pid = reply.value();
        }
        if (pid == 0 && args.size() > 6) {
            const QVariantMap hints = args.at(6).toMap();
            const QVariant p = hints.value(QStringLiteral("sender-pid"));
            if (p.canConvert<qulonglong>())
                pid = p.toULongLong();
        }
        if (pid != 0)
            emit notificationReceived(pid);

        QDBusMessage reply = message.createReply();
        reply << m_nextId++;
        connection.send(reply);
        return true;
    }

    if (member == QLatin1String("CloseNotification")) {
        connection.send(message.createReply());
        return true;
    }

    return false; // let Qt answer UnknownMethod
}

QString NotificationDaemon::introspect(const QString &path) const
{
    Q_UNUSED(path)
    // The interface definition lives in protocol/org.freedesktop.Notifications.xml
    // (embedded as a Qt resource), so it stays in sync with the protocol file.
    QFile f(QStringLiteral(":/protocol/org.freedesktop.Notifications.xml"));
    if (f.open(QIODevice::ReadOnly))
        return QString::fromUtf8(f.readAll());
    return {};
}
