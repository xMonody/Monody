#pragma once

#include "ModuleBase.h"

#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QMap>

/**
 * Minimal org.bluez.Agent1 implementation with "NoInputNoOutput" capability.
 *
 * BlueZ refuses to pair an unpaired device unless an agent is registered:
 * Device1.Connect on a never-paired device triggers pairing, and with no
 * agent the request fails.  NoInputNoOutput auto-approves every request
 * (AuthorizeService returns an empty reply = allow), which is the same as
 * bluetoothctl's `default-agent` for Just Works devices (headsets, mice,
 * keyboards, ...).
 */
class BluetoothAgent : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.Agent1")
public:
    explicit BluetoothAgent(QObject *parent = nullptr) : QObject(parent) {}

public slots:
    void Release() {}
    void Cancel() {}
    void AuthorizeService(const QDBusObjectPath &device, const QString &uuid)
    {
        Q_UNUSED(device);
        Q_UNUSED(uuid);
        // empty reply = authorise this service
    }
    QString RequestPinCode(const QDBusObjectPath &device)
    {
        Q_UNUSED(device);
        return QString();
    }
    void DisplayPinCode(const QDBusObjectPath &device, const QString &pincode)
    {
        Q_UNUSED(device);
        Q_UNUSED(pincode);
    }
    uint RequestPasskey(const QDBusObjectPath &device)
    {
        Q_UNUSED(device);
        return 0;
    }
    void DisplayPasskey(const QDBusObjectPath &device, uint passkey, qint16 entered)
    {
        Q_UNUSED(device);
        Q_UNUSED(passkey);
        Q_UNUSED(entered);
    }
    void RequestConfirmation(const QDBusObjectPath &device, uint passkey)
    {
        Q_UNUSED(device);
        Q_UNUSED(passkey);
    }
    void RequestAuthorization(const QDBusObjectPath &device)
    {
        Q_UNUSED(device);
    }
};

/**
 * Bluetooth status via org.bluez.
 *
 * A single GetManagedObjects on "/" returns every adapter and device with
 * all of their properties: the first org.bluez.Adapter1 wins (Powered /
 * Discoverable / Alias) and org.bluez.Device1 objects with Connected == true
 * are collected (Alias).  PropertiesChanged on the adapter path updates the
 * powered state instantly; the poll timer catches device connect/disconnect
 * within a few seconds.
 */
class BluetoothModule : public ModuleBase
{
    Q_OBJECT
    Q_PROPERTY(bool powered READ powered NOTIFY poweredChanged)
    Q_PROPERTY(bool discoverable READ discoverable NOTIFY discoverableChanged)
    Q_PROPERTY(QString adapterName READ adapterName NOTIFY adapterNameChanged)
    Q_PROPERTY(int connectedCount READ connectedCount NOTIFY connectedCountChanged)
    Q_PROPERTY(QStringList connectedDevices READ connectedDevices NOTIFY connectedDevicesChanged)
    Q_PROPERTY(bool hasAdapter READ hasAdapter NOTIFY hasAdapterChanged)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
    explicit BluetoothModule(QObject *parent = nullptr,
                             const QDBusConnection &bus = QDBusConnection::systemBus())
        : ModuleBase(QStringLiteral("org.bluez"), 3000, parent, bus)
    {
    }

    bool powered() const { return m_powered; }
    bool discoverable() const { return m_discoverable; }
    QString adapterName() const { return m_adapterName; }
    int connectedCount() const { return m_connectedDevices.size(); }
    QStringList connectedDevices() const { return m_connectedDevices; }
    bool hasAdapter() const { return !m_adapterPath.isEmpty(); }
    QVariantList devices() const { return m_devices; }
    QString status() const { return m_status; }

    /** Toggle the adapter (called from QML on click). */
    Q_INVOKABLE void setPowered(bool on)
    {
        setAdapterProperty(QStringLiteral("Powered"), on);
    }
    Q_INVOKABLE void setDiscoverable(bool on)
    {
        setAdapterProperty(QStringLiteral("Discoverable"), on);
    }

    /** Start/stop device discovery (sub-panel scan button). */
    Q_INVOKABLE void startDiscovery()
    {
        adapterCall(QStringLiteral("StartDiscovery"));
    }
    Q_INVOKABLE void stopDiscovery()
    {
        adapterCall(QStringLiteral("StopDiscovery"));
    }

    /** Connect / disconnect a device (click on a list entry). */
    Q_INVOKABLE void connectDevice(const QString &path)
    {
        // BR/EDR devices (TWS headsets etc.) refuse Connect with
        // br-connection-unknown until they are paired, so pair first when
        // needed, then connect automatically once pairing succeeds.
        if (devicePaired(path))
            deviceCall(path, QStringLiteral("Connect"), QStringLiteral("连接失败"));
        else
            pairThenConnect(path);
    }
    Q_INVOKABLE void disconnectDevice(const QString &path)
    {
        deviceCall(path, QStringLiteral("Disconnect"), QStringLiteral("断开失败"));
    }

signals:
    void poweredChanged();
    void discoverableChanged();
    void adapterNameChanged();
    void connectedCountChanged();
    void connectedDevicesChanged();
    void hasAdapterChanged();
    void devicesChanged();
    void statusChanged();

protected:
    void doRefresh() override
    {
        onReply(callAsync(QStringLiteral("/"),
                          QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                          QStringLiteral("GetManagedObjects")),
                [this](const QDBusPendingCallWatcher *w) {
                    const QMap<QDBusObjectPath, QVariantMap> objects = parseManagedObjects(w);

                    QString adapterPath;
                    bool powered = false, discoverable = false;
                    QString alias;
                    QStringList connected;
                    QVariantList allDevices;
                    QStringList devPaths;

                    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
                        const QString path = it.key().path();
                        const QVariantMap ifaces = it.value();
                        if (ifaces.contains(QStringLiteral("org.bluez.Adapter1"))) {
                            if (adapterPath.isEmpty()) {   // first adapter wins
                                adapterPath = path;
                                const QVariantMap p =
                                        ifaces.value(QStringLiteral("org.bluez.Adapter1")).toMap();
                                powered = p.value(QStringLiteral("Powered")).toBool();
                                discoverable = p.value(QStringLiteral("Discoverable")).toBool();
                                alias = p.value(QStringLiteral("Alias")).toString();
                            }
                        } else if (ifaces.contains(QStringLiteral("org.bluez.Device1"))) {
                            const QVariantMap p =
                                    ifaces.value(QStringLiteral("org.bluez.Device1")).toMap();
                            const QString devName = p.value(QStringLiteral("Alias")).toString();
                            const bool devConnected =
                                    p.value(QStringLiteral("Connected")).toBool();
                            const bool devPaired =
                                    p.value(QStringLiteral("Paired")).toBool();
                            if (devConnected)
                                connected << devName;

                            // Battery1 (LE Battery Service): Percentage is a
                            // byte, -1 means "this device reports no battery".
                            int battery = -1;
                            if (ifaces.contains(QStringLiteral("org.bluez.Battery1"))) {
                                const QVariantMap bp =
                                        ifaces.value(QStringLiteral("org.bluez.Battery1")).toMap();
                                if (bp.contains(QStringLiteral("Percentage")))
                                    battery = bp.value(QStringLiteral("Percentage")).toInt();
                            }

                            QVariantMap entry;
                            entry.insert(QStringLiteral("name"), devName);
                            entry.insert(QStringLiteral("connected"), devConnected);
                            entry.insert(QStringLiteral("paired"), devPaired);
                            entry.insert(QStringLiteral("path"), path);
                            entry.insert(QStringLiteral("battery"), battery);
                            allDevices.append(entry);
                            devPaths << path;
                        }
                    }

                    if (adapterPath != m_adapterPath) {
                        if (!m_adapterPath.isEmpty())
                            m_bus.disconnect(m_service, m_adapterPath,
                                             QStringLiteral("org.freedesktop.DBus.Properties"),
                                             QStringLiteral("PropertiesChanged"), this,
                                             nullptr);
                        m_adapterPath = adapterPath;
                        if (!m_adapterPath.isEmpty())
                            subscribeAdapter();
                        emit adapterNameChanged();
                        emit hasAdapterChanged();
                    }
                    updatePowered(powered);
                    updateDiscoverable(discoverable);
                    setAdapterName(alias);
                    setConnectedDevices(connected);
                    setDevices(allDevices);
                    subscribeDevices(devPaths);
                    ensureAgentRegistered();
                });
    }

private slots:
    void onDevicePropertiesChanged(const QString &iface, const QVariantMap &changed,
                                   const QStringList &invalid)
    {
        Q_UNUSED(changed);
        Q_UNUSED(invalid);
        // a device's Connected/Paired changed, or its battery level changed
        // -> refresh right away
        if (iface == QStringLiteral("org.bluez.Device1")
            || iface == QStringLiteral("org.bluez.Battery1"))
            refresh();
    }

    void onAdapterPropertiesChanged(const QString &iface, const QVariantMap &changed,
                                    const QStringList &invalid)
    {
        Q_UNUSED(invalid);
        if (iface != QStringLiteral("org.bluez.Adapter1"))
            return;
        if (changed.contains(QStringLiteral("Powered")))
            updatePowered(changed.value(QStringLiteral("Powered")).toBool());
        if (changed.contains(QStringLiteral("Discoverable")))
            updateDiscoverable(changed.value(QStringLiteral("Discoverable")).toBool());
    }

private:
    void subscribeAdapter()
    {
        // Instant feedback when the adapter state changes (also from other apps).
        m_bus.connect(m_service, m_adapterPath,
                      QStringLiteral("org.freedesktop.DBus.Properties"),
                      QStringLiteral("PropertiesChanged"), this,
                      SLOT(onAdapterPropertiesChanged(QString,QVariantMap,QStringList)));
    }

    /** Keep PropertiesChanged subscriptions in sync with the device list. */
    void subscribeDevices(const QStringList &paths)
    {
        for (const QString &old : m_subscribedDevices)
            if (!paths.contains(old))
                m_bus.disconnect(m_service, old,
                                 QStringLiteral("org.freedesktop.DBus.Properties"),
                                 QStringLiteral("PropertiesChanged"), this, nullptr);
        for (const QString &p : paths)
            if (!m_subscribedDevices.contains(p))
                m_bus.connect(m_service, p,
                              QStringLiteral("org.freedesktop.DBus.Properties"),
                              QStringLiteral("PropertiesChanged"), this,
                              SLOT(onDevicePropertiesChanged(QString,QVariantMap,QStringList)));
        m_subscribedDevices = paths;
    }

    /** Look up the cached Paired flag for a device path. */
    bool devicePaired(const QString &path) const
    {
        for (const QVariant &v : m_devices) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("path")).toString() == path)
                return m.value(QStringLiteral("paired")).toBool();
        }
        return false;
    }

    /** Pair first (needs the registered agent), then Connect. */
    void pairThenConnect(const QString &path)
    {
        if (!m_available || path.isEmpty())
            return;
        QDBusPendingCall pending = m_bus.asyncCall(QDBusMessage::createMethodCall(
            m_service, path, QStringLiteral("org.bluez.Device1"), QStringLiteral("Pair")));
        auto *watcher = new QDBusPendingCallWatcher(pending, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, path, watcher]() {
                    if (watcher->isError()) {
                        const QString err = watcher->error().message();
                        qWarning().noquote() << "bluetooth Pair failed:" << err;
                        setStatus(QStringLiteral("配对失败：") + err);
                    } else {
                        setStatus(QString());
                        deviceCall(path, QStringLiteral("Connect"),
                                   QStringLiteral("连接失败"));
                    }
                    watcher->deleteLater();
                });
    }

    /** Call a Device1 method (Connect/Disconnect) and report the outcome. */
    void deviceCall(const QString &path, const QString &method, const QString &failText)
    {
        if (!m_available || path.isEmpty())
            return;
        QDBusPendingCall pending = m_bus.asyncCall(QDBusMessage::createMethodCall(
            m_service, path, QStringLiteral("org.bluez.Device1"), method));
        auto *watcher = new QDBusPendingCallWatcher(pending, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, failText, watcher, method]() {
                    if (watcher->isError()) {
                        const QString err = watcher->error().message();
                        qWarning().noquote() << "bluetooth" << method << "failed:" << err;
                        setStatus(failText + QStringLiteral("：") + err);
                    } else {
                        setStatus(QString());
                    }
                    watcher->deleteLater();
                });
    }

    /**
     * Register the agent object once and (re)register it as BlueZ's default
     * agent.  RegisterAgent / RequestDefaultAgent are idempotent, so calling
     * this on every refresh also recovers automatically after bluez restarts.
     */
    void ensureAgentRegistered()
    {
        if (!m_available || m_adapterPath.isEmpty())
            return;

        const QString agentPath = QStringLiteral("/monodybar/bluetooth/agent");
        if (!m_agentObjectRegistered) {
            if (!m_bus.registerObject(agentPath, &m_agent, QDBusConnection::ExportAllSlots)) {
                qWarning() << "bluetooth: cannot register agent object" << agentPath;
                return;
            }
            m_agentObjectRegistered = true;
        }

        QDBusMessage reg = QDBusMessage::createMethodCall(
            m_service, QStringLiteral("/org/bluez"),
            QStringLiteral("org.bluez.AgentManager1"), QStringLiteral("RegisterAgent"));
        reg << QDBusObjectPath(agentPath) << QStringLiteral("NoInputNoOutput");

        QDBusMessage def = QDBusMessage::createMethodCall(
            m_service, QStringLiteral("/org/bluez"),
            QStringLiteral("org.bluez.AgentManager1"), QStringLiteral("RequestDefaultAgent"));
        def << QDBusObjectPath(agentPath);

        const auto reportError = [this](const QDBusPendingCall &pending, const char *what) {
            auto *w = new QDBusPendingCallWatcher(pending, this);
            connect(w, &QDBusPendingCallWatcher::finished, this, [w, what]() {
                if (w->isError()) {
                    // AlreadyExists = the agent is still registered from an
                    // earlier refresh (the expected steady state). After a
                    // bluez restart the table is empty, so this refresh
                    // registers it again without noise.
                    if (w->error().name() != QStringLiteral("org.bluez.Error.AlreadyExists"))
                        qWarning().noquote() << what << "failed:" << w->error().message();
                }
                w->deleteLater();
            });
        };
        reportError(m_bus.asyncCall(reg), "bluetooth RegisterAgent");
        reportError(m_bus.asyncCall(def), "bluetooth RequestDefaultAgent");
    }

    void setAdapterProperty(const QString &prop, bool on)
    {
        if (!m_available || m_adapterPath.isEmpty())
            return;
        QDBusMessage msg = QDBusMessage::createMethodCall(
            m_service, m_adapterPath,
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Set"));
        msg << QStringLiteral("org.bluez.Adapter1") << prop
            << QVariant::fromValue(QDBusVariant(on));
        m_bus.asyncCall(msg);
    }

    /** Call a no-arg org.bluez.Adapter1 method (StartDiscovery, ...). */
    void adapterCall(const QString &method)
    {
        if (!m_available || m_adapterPath.isEmpty())
            return;
        m_bus.asyncCall(QDBusMessage::createMethodCall(
            m_service, m_adapterPath, QStringLiteral("org.bluez.Adapter1"), method));
    }

    void updatePowered(bool v)
    {
        if (v == m_powered)
            return;
        m_powered = v;
        emit poweredChanged();
    }
    void updateDiscoverable(bool v)
    {
        if (v == m_discoverable)
            return;
        m_discoverable = v;
        emit discoverableChanged();
    }
    void setAdapterName(const QString &v)
    {
        if (v == m_adapterName)
            return;
        m_adapterName = v;
        emit adapterNameChanged();
    }
    void setConnectedDevices(const QStringList &v)
    {
        if (v == m_connectedDevices)
            return;
        m_connectedDevices = v;
        emit connectedCountChanged();
        emit connectedDevicesChanged();
    }
    void setDevices(const QVariantList &v)
    {
        if (v == m_devices)
            return;
        m_devices = v;
        emit devicesChanged();
    }
    void setStatus(const QString &v)
    {
        if (v == m_status)
            return;
        m_status = v;
        emit statusChanged();
    }

    bool m_powered = false;
    bool m_discoverable = false;
    QString m_adapterName;
    QStringList m_connectedDevices;
    QVariantList m_devices;
    QStringList m_subscribedDevices;
    QString m_status;
    QString m_adapterPath;
    BluetoothAgent m_agent;
    bool m_agentObjectRegistered = false;
};
