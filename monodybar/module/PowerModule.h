#pragma once

#include "ModuleBase.h"

/**
 * Power actions via org.freedesktop.login1 (systemd-logind).
 *
 * The four launcher footer buttons (shutdown / restart / sleep / lock) call
 * these slots. All calls are async fire-and-forget: logind performs the
 * action without needing a reply, so there is nothing to poll. `available`
 * (inherited from ModuleBase) follows the logind service owner.
 */
class PowerModule : public ModuleBase
{
    Q_OBJECT

public:
    explicit PowerModule(QObject *parent = nullptr,
                         const QDBusConnection &bus = QDBusConnection::systemBus())
        : ModuleBase(QStringLiteral("org.freedesktop.login1"), 0, parent, bus)
    {
    }

    Q_INVOKABLE void powerOff() { call(QStringLiteral("PowerOff"), true); }
    Q_INVOKABLE void reboot()   { call(QStringLiteral("Reboot"), true); }
    Q_INVOKABLE void suspend()  { call(QStringLiteral("Suspend"), true); }
    Q_INVOKABLE void lock()     { call(QStringLiteral("LockSessions"), false); }

protected:
    void doRefresh() override {}

private:
    void call(const QString &method, bool interactive)
    {
        if (!m_bus.isConnected())
            return;

        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.login1"),
            QStringLiteral("/org/freedesktop/login1"),
            QStringLiteral("org.freedesktop.login1.Manager"),
            method);
        if (interactive)
            msg << false;   // interactive = false: act immediately, no prompt
        m_bus.asyncCall(msg);
    }
};
