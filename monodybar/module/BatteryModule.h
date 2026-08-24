#pragma once

#include "ModuleBase.h"

#include <QDBusObjectPath>
#include <QMap>
#include <QtMath>

/**
 * Battery status via org.freedesktop.UPower.
 *
 * One GetManagedObjects round trip on /org/freedesktop/UPower returns every
 * device (batteries, AC adapters, ...) with all of their properties, so we
 * pick the primary power-supply battery (Type == Battery && PowerSupply)
 * without per-device calls.  PropertiesChanged on the found device and on
 * the UPower root (OnBattery) update the state instantly; the poll timer
 * stays as a safety net.
 */
class BatteryModule : public ModuleBase
{
    Q_OBJECT
    Q_PROPERTY(int percent READ percent NOTIFY percentChanged)
    Q_PROPERTY(bool charging READ charging NOTIFY chargingChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool onBattery READ onBattery NOTIFY onBatteryChanged)
    Q_PROPERTY(qint64 timeRemaining READ timeRemaining NOTIFY timeRemainingChanged)
    Q_PROPERTY(QString devicePath READ devicePath NOTIFY devicePathChanged)

public:
    explicit BatteryModule(QObject *parent = nullptr,
                           const QDBusConnection &bus = QDBusConnection::systemBus())
        : ModuleBase(QStringLiteral("org.freedesktop.UPower"), 5000, parent, bus)
    {
        // OnBattery flips the moment the AC adapter is plugged/unplugged.
        m_bus.connect(m_service,
                      QStringLiteral("/org/freedesktop/UPower"),
                      QStringLiteral("org.freedesktop.DBus.Properties"),
                      QStringLiteral("PropertiesChanged"), this,
                      SLOT(onUpowerPropertiesChanged(QString,QVariantMap,QStringList)));
    }

    int percent() const { return m_percent; }
    bool charging() const { return m_charging; }
    QString state() const { return m_state; }
    bool onBattery() const { return m_onBattery; }
    qint64 timeRemaining() const { return m_timeRemaining; }
    QString devicePath() const { return m_devicePath; }

signals:
    void percentChanged();
    void chargingChanged();
    void stateChanged();
    void onBatteryChanged();
    void timeRemainingChanged();
    void devicePathChanged();

protected:
    void doRefresh() override
    {
        // OnBattery flips when the AC adapter is plugged/unplugged.
        onReply(getPropertyAsync(QStringLiteral("/org/freedesktop/UPower"),
                                 QStringLiteral("org.freedesktop.UPower"),
                                 QStringLiteral("OnBattery")),
                [this](const QDBusPendingCallWatcher *w) {
                    const QVariant v = demarshal(arg0(w));
                    if (v.isValid())
                        setOnBattery(v.toBool());
                });
        if (m_devicePath.isEmpty())
            discoverDevice();
        else
            readDevice();
    }

private:
    void discoverDevice()
    {
        // One GetManagedObjects round trip returns every device with all
        // properties; some dbus policies deny ObjectManager, so fall back to
        // EnumerateDevices + one GetAll per device.
        onReply(callAsync(QStringLiteral("/org/freedesktop/UPower"),
                          QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                          QStringLiteral("GetManagedObjects")),
                [this](const QDBusPendingCallWatcher *w) {
                    const QMap<QDBusObjectPath, QVariantMap> objects = parseManagedObjects(w);
                    if (!objects.isEmpty()) {
                        QString found;
                        for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
                            const QVariantMap ifaces = it.value();
                            if (!ifaces.contains(QStringLiteral("org.freedesktop.UPower.Device")))
                                continue;
                            const QVariantMap props =
                                    ifaces.value(QStringLiteral("org.freedesktop.UPower.Device")).toMap();
                            // Type 2 = Battery; PowerSupply filters the
                            // primary battery out of mice/headsets etc.
                            if (props.value(QStringLiteral("Type")).toUInt() == 2
                                && props.value(QStringLiteral("PowerSupply")).toBool()) {
                                found = it.key().path();
                                applyDeviceProps(props);
                                break;
                            }
                        }
                        setDevice(found);
                        if (found.isEmpty()) {
                            // No battery installed: reset to neutral state.
                            setPercent(0);
                            setCharging(false);
                            setState(QStringLiteral("none"));
                            setTimeRemaining(0);
                        }
                        return;
                    }
                    // GetManagedObjects denied/unavailable: enumerate instead (quiet:
                    // this error is the normal fallback trigger, not a failure).
                    onReply(callAsync(QStringLiteral("/org/freedesktop/UPower"),
                                      QStringLiteral("org.freedesktop.UPower"),
                                      QStringLiteral("EnumerateDevices")),
                            [this](const QDBusPendingCallWatcher *w2) {
                                const QDBusReply<QList<QDBusObjectPath>> r2(w2->reply());
                                if (!r2.isValid())
                                    return;
                                auto pending = std::make_shared<int>(r2.value().size());
                                for (const QDBusObjectPath &p : r2.value()) {
                                    onReply(getAllAsync(p.path(),
                                                        QStringLiteral("org.freedesktop.UPower.Device")),
                                            [this, p, pending](const QDBusPendingCallWatcher *w3) {
                                                const QDBusReply<QVariantMap> r3(w3->reply());
                                                if (r3.isValid() && m_devicePath.isEmpty()) {
                                                    const QVariantMap props = r3.value();
                                                    if (props.value(QStringLiteral("Type")).toUInt() == 2
                                                        && props.value(QStringLiteral("PowerSupply")).toBool()) {
                                                        setDevice(p.path());
                                                        applyDeviceProps(props);
                                                    }
                                                }
                                                if (--(*pending) <= 0 && m_devicePath.isEmpty())
                                                    setState(QStringLiteral("none"));
                                            });
                                }
                            });
                }, true);
    }

    void setDevice(const QString &path)
    {
        if (path == m_devicePath)
            return;
        if (!m_devicePath.isEmpty())
            m_bus.disconnect(m_service, m_devicePath,
                             QStringLiteral("org.freedesktop.DBus.Properties"),
                             QStringLiteral("PropertiesChanged"), this,
                             nullptr);
        m_devicePath = path;
        if (!m_devicePath.isEmpty())
            subscribeDevice();
        emit devicePathChanged();
    }

private slots:
    void onUpowerPropertiesChanged(const QString &iface, const QVariantMap &changed,
                                   const QStringList &invalid)
    {
        Q_UNUSED(invalid);
        if (iface != QStringLiteral("org.freedesktop.UPower"))
            return;
        if (changed.contains(QStringLiteral("OnBattery")))
            setOnBattery(changed.value(QStringLiteral("OnBattery")).toBool());
    }

    void onDevicePropertiesChanged(const QString &iface, const QVariantMap &changed,
                                   const QStringList &invalid)
    {
        Q_UNUSED(invalid);
        if (iface != QStringLiteral("org.freedesktop.UPower.Device"))
            return;
        applyDeviceProps(changed);
    }

private:
    void subscribeDevice()
    {
        // Instant updates for percent / state / time while charging.
        m_bus.connect(m_service, m_devicePath,
                      QStringLiteral("org.freedesktop.DBus.Properties"),
                      QStringLiteral("PropertiesChanged"), this,
                      SLOT(onDevicePropertiesChanged(QString,QVariantMap,QStringList)));
    }

    void readDevice()
    {
        onReply(getAllAsync(m_devicePath,
                            QStringLiteral("org.freedesktop.UPower.Device")),
                [this](const QDBusPendingCallWatcher *w) {
                    const QDBusReply<QVariantMap> reply(w->reply());
                    if (!reply.isValid()) {
                        // Device vanished (battery unplugged): rediscover.
                        setDevice(QString());
                        setPercent(0);
                        setCharging(false);
                        setState(QStringLiteral("none"));
                        setTimeRemaining(0);
                        discoverDevice();
                        return;
                    }
                    applyDeviceProps(reply.value());
                });
    }

    void applyDeviceProps(const QVariantMap &props)
    {
        const QVariant pct = props.value(QStringLiteral("Percentage"));
        if (pct.isValid())
            setPercent(int(qRound(pct.toDouble())));

        const QVariant st = props.value(QStringLiteral("State"));
        if (st.isValid())
            setState(stateName(st.toUInt()));

        const bool isCharging = m_state == QLatin1String("charging")
                                || m_state == QLatin1String("full")
                                || m_state == QLatin1String("pending");
        setCharging(isCharging);

        const QVariant t = props.value(QStringLiteral("TimeToEmpty"));
        if (t.isValid())
            setTimeRemaining(t.toLongLong());

        const QVariant tFull = props.value(QStringLiteral("TimeToFull"));
        if (tFull.isValid() && isCharging)
            setTimeRemaining(tFull.toLongLong());
    }

    static QString stateName(uint s)
    {
        switch (s) {
        case 1:  return QStringLiteral("charging");
        case 2:  return QStringLiteral("discharging");
        case 3:  return QStringLiteral("empty");
        case 4:  return QStringLiteral("full");
        case 5:  return QStringLiteral("pending");
        case 6:  return QStringLiteral("pending");
        default: return QStringLiteral("unknown");
        }
    }

    void setPercent(int v)
    {
        v = qBound(0, v, 100);
        if (v == m_percent)
            return;
        m_percent = v;
        emit percentChanged();
    }
    void setCharging(bool v)
    {
        if (v == m_charging)
            return;
        m_charging = v;
        emit chargingChanged();
    }
    void setState(const QString &v)
    {
        if (v == m_state)
            return;
        m_state = v;
        emit stateChanged();
    }
    void setOnBattery(bool v)
    {
        if (v == m_onBattery)
            return;
        m_onBattery = v;
        emit onBatteryChanged();
    }
    void setTimeRemaining(qint64 v)
    {
        if (v == m_timeRemaining)
            return;
        m_timeRemaining = v;
        emit timeRemainingChanged();
    }

    int m_percent = 0;
    bool m_charging = false;
    QString m_state = QStringLiteral("unknown");
    bool m_onBattery = true;
    qint64 m_timeRemaining = 0;
    QString m_devicePath;
};
