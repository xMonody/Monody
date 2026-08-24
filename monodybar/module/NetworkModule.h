#pragma once

#include "ModuleBase.h"

#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>

/**
 * Network status via org.freedesktop.NetworkManager.
 *
 * No polling: NetworkManager broadcasts PropertiesChanged for every state
 * change, so the module reads once at startup (and whenever the daemon
 * appears) and then updates from signals on the NM root, the active
 * connection, the devices and the access point.
 *
 * Wired vs wifi is decided by the DeviceType of the devices
 * (org.freedesktop.NetworkManager.Device.DeviceType: 1 = ethernet,
 * 2 = wifi).  When NM has no PrimaryConnection (e.g. the link is managed
 * outside NetworkManager) the devices are probed directly, so a live
 * ethernet link still shows the wired icon.
 */
class NetworkModule : public ModuleBase
{
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(QString type READ type NOTIFY typeChanged)          // wired / wifi / vpn / none
    Q_PROPERTY(QString name READ name NOTIFY nameChanged)          // connection Id (SSID for wifi)
    Q_PROPERTY(int strength READ strength NOTIFY strengthChanged)  // 0..100 wifi AP strength, -1 = n/a
    Q_PROPERTY(QString connectivity READ connectivity NOTIFY connectivityChanged) // full/limited/portal/none/unknown
    Q_PROPERTY(bool wirelessEnabled READ wirelessEnabled NOTIFY wirelessEnabledChanged)
    Q_PROPERTY(bool wirelessHardwareEnabled READ wirelessHardwareEnabled
                   NOTIFY wirelessHardwareEnabledChanged)
    Q_PROPERTY(bool hasWifiDevice READ hasWifiDevice NOTIFY hasWifiDeviceChanged)
    Q_PROPERTY(QVariantList wifiNetworks READ wifiNetworks NOTIFY wifiNetworksChanged)

public:
    explicit NetworkModule(QObject *parent = nullptr,
                           const QDBusConnection &bus = QDBusConnection::systemBus())
        : ModuleBase(QStringLiteral("org.freedesktop.NetworkManager"), 0, parent, bus)
    {
        // NM root: PrimaryConnection / PrimaryConnectionType / Connectivity /
        // WirelessEnabled changes -> re-read everything.
        m_bus.connect(m_service,
                      QStringLiteral("/org/freedesktop/NetworkManager"),
                      QStringLiteral("org.freedesktop.DBus.Properties"),
                      QStringLiteral("PropertiesChanged"), this,
                      SLOT(onPropertiesChanged(QString,QVariantMap,QStringList)));
        // device list changes (plug/unplug) -> re-read
        m_bus.connect(m_service,
                      QStringLiteral("/org/freedesktop/NetworkManager"),
                      m_service, QStringLiteral("DevicesChanged"), this,
                      SLOT(onDevicesChanged()));
    }

    bool connected() const { return m_connected; }
    QString type() const { return m_type; }
    QString name() const { return m_name; }
    int strength() const { return m_strength; }
    QString connectivity() const { return m_connectivity; }
    bool wirelessEnabled() const { return m_wirelessEnabled; }
    bool wirelessHardwareEnabled() const { return m_wirelessHardwareEnabled; }
    bool hasWifiDevice() const { return m_hasWifiDevice; }
    QVariantList wifiNetworks() const { return m_wifiNetworks; }

    /** Toggle the wifi radio (called from QML on click). */
    Q_INVOKABLE void setWirelessEnabled(bool on)
    {
        if (!m_available)
            return;
        m_bus.asyncCall(QDBusMessage::createMethodCall(
            m_service, QStringLiteral("/org/freedesktop/NetworkManager"),
            m_service, QStringLiteral("SetWirelessEnabled")) << on);
    }

    /** Ask NM to rescan, then re-read the network list (sub-panel refresh). */
    Q_INVOKABLE void scanWifi()
    {
        if (!m_available || m_wifiDevicePath.isEmpty())
            return;
        m_bus.asyncCall(QDBusMessage::createMethodCall(
            m_service, m_wifiDevicePath,
            QStringLiteral("org.freedesktop.NetworkManager.Device.Wireless"),
            QStringLiteral("RequestScan")) << QVariantMap());
        QTimer::singleShot(2000, this, [this] { refresh(); });
    }

signals:
    void connectedChanged();
    void typeChanged();
    void nameChanged();
    void strengthChanged();
    void connectivityChanged();
    void wirelessEnabledChanged();
    void wirelessHardwareEnabledChanged();
    void hasWifiDeviceChanged();
    void wifiNetworksChanged();

private slots:
    void onPropertiesChanged(const QString &iface, const QVariantMap &changed,
                             const QStringList &invalid)
    {
        Q_UNUSED(changed);
        Q_UNUSED(invalid);
        // any of our subscribed objects changed -> re-read
        if (iface == QStringLiteral("org.freedesktop.NetworkManager")
            || iface == QStringLiteral("org.freedesktop.NetworkManager.Connection.Active")
            || iface == QStringLiteral("org.freedesktop.NetworkManager.Device")
            || iface == QStringLiteral("org.freedesktop.NetworkManager.Device.Wireless")
            || iface == QStringLiteral("org.freedesktop.NetworkManager.AccessPoint"))
            refresh();
    }

    void onDevicesChanged()
    {
        refresh();
    }

protected:
    void doRefresh() override
    {
        onReply(getAllAsync(QStringLiteral("/org/freedesktop/NetworkManager"),
                            QStringLiteral("org.freedesktop.NetworkManager")),
                [this](const QDBusPendingCallWatcher *w) {
                    const QDBusReply<QVariantMap> reply(w->reply());
                    if (!reply.isValid())
                        return;
                    const QVariantMap props = reply.value();

                    updateWirelessEnabled(props.value(QStringLiteral("WirelessEnabled")).toBool());
                    updateWirelessHardwareEnabled(
                            props.value(QStringLiteral("WirelessHardwareEnabled")).toBool());
                    setConnectivity(connectivityName(
                            props.value(QStringLiteral("Connectivity")).toUInt()));

                    const QString active =
                            dbusPath(props.value(QStringLiteral("PrimaryConnection")));
                    const QString nmTypeHint =
                            props.value(QStringLiteral("PrimaryConnectionType")).toString();
                    subscribePath(m_activeConn, active);
                    scanDevices(active, nmTypeHint);
                });
    }

    // Enumerate every NM device: track wifi presence for the quick-settings
    // button, subscribe them for signals, and - when NM has no primary
    // connection - decide wired/wifi from the device links (an unmanaged
    // ethernet link still counts as wired).
    void scanDevices(const QString &active, const QString &nmTypeHint)
    {
        onReply(callAsync(QStringLiteral("/org/freedesktop/NetworkManager"),
                          QStringLiteral("org.freedesktop.NetworkManager"),
                          QStringLiteral("GetDevices")),
                [this, active, nmTypeHint](const QDBusPendingCallWatcher *w) {
                    const QDBusReply<QList<QDBusObjectPath>> r(w->reply());
                    if (!r.isValid()) {
                        setHasWifiDevice(false);
                        if (active.isEmpty() || active == QLatin1String("/")) {
                            setConnected(false);
                            setType(nmTypeHint.isEmpty()
                                    ? QStringLiteral("none") : typeFromNm(nmTypeHint));
                            setName(QString());
                            setStrength(-1);
                        } else {
                            setConnected(true);
                            setType(typeFromNm(nmTypeHint));
                        }
                        return;
                    }
                    QStringList paths;
                    for (const QDBusObjectPath &d : r.value())
                        paths << d.path();
                    subscribeDevices(paths);

                    struct Probe {
                        int total = 0;
                        int done = 0;
                        bool sawWifi = false;
                        bool ethUp = false;
                        QString wifiDev;
                    };
                    auto probe = std::make_shared<Probe>();
                    probe->total = paths.size();
                    for (const QString &p : paths) {
                        onReply(getAllAsync(p, QStringLiteral("org.freedesktop.NetworkManager.Device")),
                                [this, p, active, nmTypeHint, probe](const QDBusPendingCallWatcher *w2) {
                                    const QDBusReply<QVariantMap> r2(w2->reply());
                                    if (r2.isValid()) {
                                        const QVariantMap m = r2.value();
                                        const uint devType =
                                                m.value(QStringLiteral("DeviceType")).toUInt();
                                        const uint devState =
                                                m.value(QStringLiteral("State")).toUInt();
                                        if (devType == 2) {
                                            probe->sawWifi = true;
                                            if (devState == 100 && probe->wifiDev.isEmpty())
                                                probe->wifiDev = p;
                                        } else if (devType == 1
                                                   && devState != 20   // unavailable
                                                   && devState != 120) { // failed
                                            probe->ethUp = true;
                                        }
                                    }
                                    if (++probe->done < probe->total)
                                        return;
                                    setHasWifiDevice(probe->sawWifi);

                                    if (active.isEmpty() || active == QLatin1String("/")) {
                                        // no NM-managed connection: the device
                                        // links decide what to show
                                        setName(QString());
                                        if (probe->ethUp) {
                                            setConnected(true);
                                            setType(QStringLiteral("wired"));
                                            setStrength(-1);
                                            subscribePath(m_accessPoint, QString());
                                            clearWifiNetworks();
                                        } else if (!probe->wifiDev.isEmpty()) {
                                            setConnected(true);
                                            setType(QStringLiteral("wifi"));
                                            m_wifiDevicePath = probe->wifiDev;
                                            queryWifiStrength(probe->wifiDev);
                                            refreshWifiNetworks(probe->wifiDev);
                                        } else {
                                            setConnected(false);
                                            setType(nmTypeHint.isEmpty()
                                                    ? QStringLiteral("none")
                                                    : typeFromNm(nmTypeHint));
                                            setStrength(-1);
                                            subscribePath(m_accessPoint, QString());
                                        }
                                        return;
                                    }

                                    setConnected(true);
                                    readActiveConnection(active, nmTypeHint);
                                });
                    }
                });
    }

    // Read the active connection's Id + Devices; the connection's device
    // types decide wired vs wifi.
    void readActiveConnection(const QString &active, const QString &nmTypeHint)
    {
        onReply(getAllAsync(active,
                            QStringLiteral("org.freedesktop.NetworkManager.Connection.Active")),
                [this, nmTypeHint](const QDBusPendingCallWatcher *w2) {
                    const QDBusReply<QVariantMap> r2(w2->reply());
                    if (!r2.isValid())
                        return;
                    const QVariantMap p2 = r2.value();
                    setName(p2.value(QStringLiteral("Id")).toString());
                    const QStringList devs = dbusStringList(
                            p2.value(QStringLiteral("Devices")));
                    if (devs.isEmpty()) {
                        setType(typeFromNm(nmTypeHint));
                        return;
                    }
                    probeDevices(devs, nmTypeHint);
                });
    }

private:
    // Inspect every device of the active connection: any wifi device wins,
    // otherwise an ethernet device makes it wired, otherwise fall back to
    // the NM PrimaryConnectionType string.
    void probeDevices(const QStringList &devs, const QString &nmTypeHint)
    {
        subscribeDevices(devs);
        struct Probe {
            int total = 0;
            int done = 0;
            QString wifiDev;
            bool sawEthernet = false;
        };
        auto probe = std::make_shared<Probe>();
        probe->total = devs.size();
        for (const QString &d : devs) {
            onReply(getAllAsync(d, QStringLiteral("org.freedesktop.NetworkManager.Device")),
                    [this, d, nmTypeHint, probe](const QDBusPendingCallWatcher *w) {
                        const QDBusReply<QVariantMap> r(w->reply());
                        if (r.isValid()) {
                            // DeviceType: 1 = ethernet, 2 = wifi
                            const uint devType =
                                    r.value().value(QStringLiteral("DeviceType")).toUInt();
                            if (devType == 2 && probe->wifiDev.isEmpty())
                                probe->wifiDev = d;
                            else if (devType == 1)
                                probe->sawEthernet = true;
                        }
                        if (++probe->done < probe->total)
                            return;
                        // all devices inspected: decide now
                        if (!probe->wifiDev.isEmpty()) {
                            setType(QStringLiteral("wifi"));
                            m_wifiDevicePath = probe->wifiDev;
                            queryWifiStrength(probe->wifiDev);
                            refreshWifiNetworks(probe->wifiDev);
                        } else if (probe->sawEthernet) {
                            setType(QStringLiteral("wired"));
                            setStrength(-1);
                            subscribePath(m_accessPoint, QString());
                            clearWifiNetworks();
                        } else {
                            setType(typeFromNm(nmTypeHint));
                        }
                    });
        }
    }

    void queryWifiStrength(const QString &devPath)
    {
        onReply(getAllAsync(devPath,
                            QStringLiteral("org.freedesktop.NetworkManager.Device.Wireless")),
                [this, devPath](const QDBusPendingCallWatcher *w) {
                    const QDBusReply<QVariantMap> r(w->reply());
                    if (!r.isValid())
                        return;
                    const QString ap =
                            dbusPath(r.value().value(QStringLiteral("ActiveAccessPoint")));
                    if (ap.isEmpty() || ap == QLatin1String("/")) {
                        m_activeAp.clear();
                        setStrength(-1);
                        subscribePath(m_accessPoint, QString());
                        return;
                    }
                    m_activeAp = ap;
                    onReply(getAllAsync(ap,
                                        QStringLiteral("org.freedesktop.NetworkManager.AccessPoint")),
                            [this, ap](const QDBusPendingCallWatcher *w2) {
                                subscribePath(m_accessPoint, ap);
                                const QDBusReply<QVariantMap> r2(w2->reply());
                                if (!r2.isValid())
                                    return;
                                setStrength(r2.value()
                                                    .value(QStringLiteral("Strength"))
                                                    .toInt());
                            });
                });
    }

    // Enumerate the wifi device's access points into m_wifiNetworks:
    // [{name, strength, active}], strongest first (capped).
    void refreshWifiNetworks(const QString &devPath)
    {
        onReply(callAsync(devPath,
                          QStringLiteral("org.freedesktop.NetworkManager.Device.Wireless"),
                          QStringLiteral("GetAllNetworks")),
                [this](const QDBusPendingCallWatcher *w) {
                    const QDBusReply<QList<QDBusObjectPath>> r(w->reply());
                    if (!r.isValid())
                        return;
                    const QList<QDBusObjectPath> aps = r.value();
                    struct Probe {
                        int total = 0;
                        int done = 0;
                        QList<QVariantMap> nets;
                    };
                    auto probe = std::make_shared<Probe>();
                    probe->total = aps.size();
                    for (const QDBusObjectPath &ap : aps) {
                        onReply(getAllAsync(ap.path(),
                                            QStringLiteral("org.freedesktop.NetworkManager.AccessPoint")),
                                [this, ap, probe](const QDBusPendingCallWatcher *w2) {
                                    const QDBusReply<QVariantMap> r2(w2->reply());
                                    if (r2.isValid()) {
                                        const QVariantMap m = r2.value();
                                        const QByteArray ssid =
                                                m.value(QStringLiteral("Ssid")).toByteArray();
                                        if (!ssid.isEmpty()) {
                                            QVariantMap entry;
                                            entry.insert(QStringLiteral("name"),
                                                         QString::fromUtf8(ssid));
                                            entry.insert(QStringLiteral("strength"),
                                                         m.value(QStringLiteral("Strength")).toInt());
                                            entry.insert(QStringLiteral("active"),
                                                         ap.path() == m_activeAp);
                                            probe->nets.append(entry);
                                        }
                                    }
                                    if (++probe->done < probe->total)
                                        return;
                                    std::sort(probe->nets.begin(), probe->nets.end(),
                                              [](const QVariantMap &a, const QVariantMap &b) {
                                                  return a.value(QStringLiteral("strength")).toInt()
                                                         > b.value(QStringLiteral("strength")).toInt();
                                              });
                                    if (probe->nets.size() > 40)
                                        probe->nets.resize(40);
                                    QVariantList list;
                                    for (const QVariantMap &e : probe->nets)
                                        list.append(e);
                                    setWifiNetworks(list);
                                });
                    }
                });
    }

    void clearWifiNetworks()
    {
        m_wifiDevicePath.clear();
        m_activeAp.clear();
        setWifiNetworks({});
    }

    void setWifiNetworks(const QVariantList &v)
    {
        if (v == m_wifiNetworks)
            return;
        m_wifiNetworks = v;
        emit wifiNetworksChanged();
    }

    void updateWirelessHardwareEnabled(bool v)
    {
        if (v == m_wirelessHardwareEnabled)
            return;
        m_wirelessHardwareEnabled = v;
        emit wirelessHardwareEnabledChanged();
    }

    void setHasWifiDevice(bool v)
    {
        if (v == m_hasWifiDevice)
            return;
        m_hasWifiDevice = v;
        emit hasWifiDeviceChanged();
    }

    /** (Re)subscribe PropertiesChanged on `path`; empty path clears it. */
    void subscribePath(QString &current, const QString &path)
    {
        if (path == current)
            return;
        if (!current.isEmpty())
            m_bus.disconnect(m_service, current,
                             QStringLiteral("org.freedesktop.DBus.Properties"),
                             QStringLiteral("PropertiesChanged"), this, nullptr);
        current = path;
        if (!current.isEmpty())
            m_bus.connect(m_service, current,
                          QStringLiteral("org.freedesktop.DBus.Properties"),
                          QStringLiteral("PropertiesChanged"), this,
                          SLOT(onPropertiesChanged(QString,QVariantMap,QStringList)));
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
                              SLOT(onPropertiesChanged(QString,QVariantMap,QStringList)));
        m_subscribedDevices = paths;
    }

    static QString typeFromNm(const QString &nmType)
    {
        if (nmType == QLatin1String("802-3-ethernet"))
            return QStringLiteral("wired");
        if (nmType == QLatin1String("802-11-wireless"))
            return QStringLiteral("wifi");
        if (nmType == QLatin1String("vpn"))
            return QStringLiteral("vpn");
        return nmType.isEmpty() ? QStringLiteral("none") : QStringLiteral("unknown");
    }

    static QString connectivityName(uint c)
    {
        switch (c) {
        case 0:  return QStringLiteral("unknown");
        case 1:  return QStringLiteral("none");
        case 2:  return QStringLiteral("portal");
        case 3:  return QStringLiteral("limited");
        case 4:  return QStringLiteral("full");
        default: return QStringLiteral("unknown");
        }
    }

    void setConnected(bool v)
    {
        if (v == m_connected)
            return;
        m_connected = v;
        emit connectedChanged();
    }
    void setType(const QString &v)
    {
        if (v == m_type)
            return;
        m_type = v;
        emit typeChanged();
    }
    void setName(const QString &v)
    {
        if (v == m_name)
            return;
        m_name = v;
        emit nameChanged();
    }
    void setStrength(int v)
    {
        v = qBound(-1, v, 100);
        if (v == m_strength)
            return;
        m_strength = v;
        emit strengthChanged();
    }
    void setConnectivity(const QString &v)
    {
        if (v == m_connectivity)
            return;
        m_connectivity = v;
        emit connectivityChanged();
    }
    void updateWirelessEnabled(bool v)
    {
        if (v == m_wirelessEnabled)
            return;
        m_wirelessEnabled = v;
        emit wirelessEnabledChanged();
    }

    bool m_connected = false;
    QString m_type = QStringLiteral("none");
    QString m_name;
    int m_strength = -1;
    QString m_connectivity = QStringLiteral("unknown");
    bool m_wirelessEnabled = false;
    bool m_wirelessHardwareEnabled = false;
    bool m_hasWifiDevice = false;
    QVariantList m_wifiNetworks;
    QString m_activeConn;          // subscribed PropertiesChanged paths
    QString m_accessPoint;
    QString m_wifiDevicePath;
    QString m_activeAp;
    QStringList m_subscribedDevices;
};
