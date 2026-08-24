#pragma once

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariant>

/**
 * Shared plumbing for the system-bus status modules
 * (BatteryModule / NetworkModule / BluetoothModule in module/).
 *
 * Every module talks to one D-Bus service:
 *   * watches the service owner so `available` follows the daemon and a
 *     refresh is triggered the moment it (re)appears;
 *   * polls on a timer (interval configurable per module);
 *   * exposes its state as read-only Q_PROPERTYs that qml/main.qml binds to.
 *
 * All D-Bus traffic is asynchronous (QDBusPendingCall + watcher), so the
 * polling never blocks the UI thread.
 *
 * The modules are header-only QObjects: AUTOMOC mocs them because they are
 * listed in CMakeLists.txt and included by src/main.cpp.
 */
class ModuleBase : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(int refreshInterval READ refreshInterval WRITE setRefreshInterval
                   NOTIFY refreshIntervalChanged)

public:
    explicit ModuleBase(const QString &service, int refreshMs, QObject *parent = nullptr,
                        const QDBusConnection &bus = QDBusConnection::systemBus())
        : QObject(parent), m_service(service), m_bus(bus)
    {
        // refreshMs <= 0 disables polling: the module updates only from
        // signals and one read at startup (see refresh()).
        if (refreshMs > 0)
            m_refreshTimer.setInterval(refreshMs);
        m_refreshTimer.setSingleShot(false);
        connect(&m_refreshTimer, &QTimer::timeout, this, &ModuleBase::refresh);

        m_watcher = new QDBusServiceWatcher(
            service, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(m_watcher, &QDBusServiceWatcher::serviceOwnerChanged,
                this, &ModuleBase::onServiceOwnerChanged);
        checkService();
    }

    bool available() const { return m_available; }
    int refreshInterval() const { return m_refreshTimer.interval(); }

    void setRefreshInterval(int ms)
    {
        if (ms < 0 || ms == m_refreshTimer.interval())
            return;
        m_refreshTimer.setInterval(ms);
        emit refreshIntervalChanged();
    }

    /** Trigger one refresh now; (re)starts the polling timer when enabled. */
    Q_INVOKABLE void refresh()
    {
        if (m_refreshTimer.interval() > 0)
            m_refreshTimer.start();
        if (m_bus.isConnected())
            doRefresh();
    }

signals:
    void availableChanged();
    void refreshIntervalChanged();

protected:
    virtual void doRefresh() = 0;

    /** Build an async method call to m_service; extra args are appended. */
    template <typename... Args>
    QDBusPendingCall callAsync(const QString &path, const QString &iface,
                               const QString &method, const Args &...args)
    {
        QDBusMessage msg = QDBusMessage::createMethodCall(m_service, path, iface, method);
        QList<QVariant> a;
        (a.append(QVariant::fromValue(args)), ...);
        msg.setArguments(a);
        return m_bus.asyncCall(msg);
    }

    /** org.freedesktop.DBus.Properties.Get for one property (reply: QDBusVariant). */
    QDBusPendingCall getPropertyAsync(const QString &path, const QString &iface,
                                      const QString &prop) const
    {
        QDBusMessage msg = QDBusMessage::createMethodCall(
            m_service, path, QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("Get"));
        msg << iface << prop;
        return m_bus.asyncCall(msg);
    }

    /** org.freedesktop.DBus.Properties.GetAll (reply: QVariantMap, values unwrapped). */
    QDBusPendingCall getAllAsync(const QString &path, const QString &iface) const
    {
        QDBusMessage msg = QDBusMessage::createMethodCall(
            m_service, path, QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("GetAll"));
        msg << iface;
        return m_bus.asyncCall(msg);
    }

    /**
     * Run fn(watcher) when `pending` finishes, on success and on error
     * alike - the battery module's fallback is driven by an error reply, so
     * the callback must always fire.  Errors are logged once here (unless
     * `quiet`), while callers skip them via reply.isValid().
     */
    template <typename Fn>
    void onReply(const QDBusPendingCall &pending, Fn &&fn, bool quiet = false)
    {
        auto *watcher = new QDBusPendingCallWatcher(pending, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [watcher, fn = std::forward<Fn>(fn), quiet]() mutable {
                    if (watcher->isError() && !quiet)
                        qWarning().noquote() << "module dbus:" << watcher->error().name()
                                             << watcher->error().message();
                    fn(watcher);
                    watcher->deleteLater();
                });
    }

    /** First reply argument (Properties.Get returns (v) -> QDBusVariant). */
    static QVariant arg0(const QDBusPendingCallWatcher *w)
    {
        const QList<QVariant> args = w->reply().arguments();
        return args.isEmpty() ? QVariant() : args.constFirst();
    }

    /** Unwrap a top-level D-Bus variant; passes plain values through. */
    static QVariant demarshal(const QVariant &v)
    {
        if (v.canConvert<QDBusVariant>())
            return qvariant_cast<QDBusVariant>(v).variant();
        return v;
    }

    /** ao inside a{sv}: usually QStringList, sometimes QDBusArgument. */
    static QStringList dbusStringList(const QVariant &v)
    {
        if (v.canConvert<QStringList>())
            return v.toStringList();
        if (v.canConvert<QDBusArgument>()) {
            QDBusArgument arg = v.value<QDBusArgument>();
            if (arg.currentSignature() == QLatin1String("ao")) {
                QStringList out;
                arg.beginArray();
                while (!arg.atEnd()) {
                    QDBusObjectPath p;
                    arg >> p;
                    out << p.path();
                }
                arg.endArray();
                return out;
            }
        }
        return {};
    }

    /** o inside a{sv}: QDBusObjectPath (toString() on it is empty). */
    static QString dbusPath(const QVariant &v)
    {
        if (v.canConvert<QDBusObjectPath>())
            return qvariant_cast<QDBusObjectPath>(v).path();
        if (v.canConvert<QString>())
            return v.toString();
        return {};
    }

    /**
     * Parse an org.freedesktop.DBus.ObjectManager.GetManagedObjects reply
     * (a{oa{sa{sv}}}). Qt 6.10 cannot demarshal this signature into a
     * single QMap<QDBusObjectPath, QVariantMap>, so read it with the
     * correctly-typed nested map and flatten it.
     */
    static QMap<QDBusObjectPath, QVariantMap> parseManagedObjects(
            const QDBusPendingCallWatcher *w)
    {
        using DeepMap = QMap<QDBusObjectPath, QMap<QString, QVariantMap>>;

        QMap<QDBusObjectPath, QVariantMap> objects;
        const QList<QVariant> args = w->reply().arguments();
        if (args.isEmpty())
            return objects;
        const QVariant &v = args.constFirst();

        DeepMap deep;
        if (v.canConvert<DeepMap>()) {
            deep = v.value<DeepMap>();
        } else if (v.canConvert<QDBusArgument>()) {
            QDBusArgument arg = v.value<QDBusArgument>();
            arg >> deep;
        } else {
            return objects;
        }

        for (auto it = deep.cbegin(); it != deep.cend(); ++it) {
            QVariantMap ifaces;
            for (auto jt = it.value().cbegin(); jt != it.value().cend(); ++jt) {
                QVariantMap props;
                for (auto pit = jt.value().cbegin(); pit != jt.value().cend(); ++pit)
                    props.insert(pit.key(), demarshal(pit.value()));
                ifaces.insert(jt.key(), props);
            }
            objects.insert(it.key(), ifaces);
        }
        return objects;
    }

protected:
    QString m_service;
    QDBusConnection m_bus;
    QTimer m_refreshTimer;
    bool m_available = false;

private slots:
    void onServiceOwnerChanged(const QString &service, const QString &oldOwner,
                               const QString &newOwner)
    {
        Q_UNUSED(service);
        Q_UNUSED(oldOwner);
        const bool up = !newOwner.isEmpty();
        if (up != m_available) {
            m_available = up;
            emit availableChanged();
        }
        if (up)
            refresh();
    }

private:
    void checkService()
    {
        // Ask dbus-daemon who owns the service right now; the watcher keeps
        // us in sync afterwards. An error simply means "not owned yet".
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.DBus"),
            QStringLiteral("/org/freedesktop/DBus"),
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
        msg << m_service;
        onReply(m_bus.asyncCall(msg), [this](const QDBusPendingCallWatcher *w) {
            if (w->isError())
                return;   // not owned (yet); NameOwnerChanged will catch it
            const QVariant v = demarshal(arg0(w));
            const bool up = v.isValid() && !v.toString().isEmpty();
            if (up != m_available) {
                m_available = up;
                emit availableChanged();
            }
            if (up)
                refresh();
        });
    }

    QDBusServiceWatcher *m_watcher = nullptr;
};
