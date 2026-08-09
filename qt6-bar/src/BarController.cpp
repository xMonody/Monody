#include "BarController.h"

#include "IconFinder.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QProcess>
#include <QRegularExpression>

Q_LOGGING_CATEGORY(lcBar, "bar.socket")

// ---------------------------------------------------------------------------
// WindowListModel
// ---------------------------------------------------------------------------

int WindowListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_items.size();
}

QVariant WindowListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const WindowInfo &item = m_items.at(index.row());
    switch (role) {
    case IdRole:
        return item.id;
    case AppIdRole:
        return item.appId;
    default:
        return {};
    }
}

QHash<int, QByteArray> WindowListModel::roleNames() const
{
    return {
        {IdRole, "id"},
        {AppIdRole, "appId"},
    };
}

int WindowListModel::indexOf(int id) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).id == id)
            return i;
    }
    return -1;
}

void WindowListModel::addWindow(int id, const QString &appId)
{
    const int i = indexOf(id);
    if (i >= 0) {
        if (m_items[i].appId == appId)
            return;
        m_items[i].appId = appId;
        emit dataChanged(index(i), index(i), {AppIdRole});
        return;
    }
    beginInsertRows(QModelIndex(), m_items.size(), m_items.size());
    m_items.append({id, appId});
    endInsertRows();
}

void WindowListModel::removeWindow(int id)
{
    const int i = indexOf(id);
    if (i < 0)
        return;
    beginRemoveRows(QModelIndex(), i, i);
    m_items.removeAt(i);
    endRemoveRows();
}

void WindowListModel::clear()
{
    if (m_items.isEmpty())
        return;
    beginResetModel();
    m_items.clear();
    endResetModel();
}

// ---------------------------------------------------------------------------
// BarController
// ---------------------------------------------------------------------------

BarController::BarController(QObject *parent)
    : QObject(parent)
{
    m_reconnectTimer.setInterval(1000);
    connect(&m_windows, &QAbstractItemModel::rowsInserted, this, &BarController::windowCountChanged);
    connect(&m_windows, &QAbstractItemModel::rowsRemoved, this, &BarController::windowCountChanged);
    connect(&m_windows, &QAbstractItemModel::modelReset, this, &BarController::windowCountChanged);
}

void BarController::setSocketPath(const QString &path)
{
    if (m_socketPath == path)
        return;
    m_socketPath = path;
    emit socketPathChanged();
}

void BarController::setDebugMode(bool on)
{
    if (m_debugMode == on)
        return;
    m_debugMode = on;
    emit debugModeChanged();
}

void BarController::start()
{
    connect(&m_reconnectTimer, &QTimer::timeout, this, &BarController::tryConnect);
    connect(&m_socket, &QLocalSocket::connected, this, &BarController::onSocketConnected);
    connect(&m_socket, &QLocalSocket::disconnected, this, &BarController::onSocketDisconnected);
    connect(&m_socket, &QLocalSocket::readyRead, this, &BarController::onReadyRead);
    tryConnect();
}

void BarController::tryConnect()
{
    if (m_socket.state() != QLocalSocket::UnconnectedState)
        return;
    m_socket.connectToServer(m_socketPath);
}

void BarController::onSocketConnected()
{
    m_connected = true;
    m_reconnectTimer.stop();
    qCDebug(lcBar) << "connected to" << m_socketPath;
    emit connectedChanged();
}

void BarController::onSocketDisconnected()
{
    m_buffer.clear();
    if (m_connected) {
        m_connected = false;
        emit connectedChanged();
        qCDebug(lcBar) << "disconnected; will retry";
    }
    if (!m_reconnectTimer.isActive())
        m_reconnectTimer.start();
}

void BarController::onReadyRead()
{
    m_buffer += m_socket.readAll();
    extractMessages();
}

/**
 * The compositor sends newline-delimited JSON. We also tolerate a stream
 * without newlines (single or concatenated JSON objects) so that other
 * compositors work out of the box.
 */
void BarController::extractMessages()
{
    // 1) newline-delimited messages
    int nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(nl).trimmed();
        m_buffer.remove(0, nl + 1);
        if (!line.isEmpty())
            handleLine(line);
    }

    // 2) leftover without newline: peel complete JSON objects
    while (!m_buffer.isEmpty()) {
        int brace = -1;
        while ((brace = m_buffer.indexOf('}', brace + 1)) >= 0) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(m_buffer.left(brace + 1), &err);
            if (err.error == QJsonParseError::NoError && doc.isObject()) {
                handleLine(m_buffer.left(brace + 1));
                m_buffer.remove(0, brace + 1);
                m_buffer = m_buffer.trimmed();
                brace = -1; // restart the scan for the next object
            }
        }
        if (brace == -1) {
            // No complete object: keep buffering if it might grow into one,
            // otherwise drop the garbage so we cannot spin forever.
            QJsonParseError err;
            QJsonDocument::fromJson(m_buffer, &err);
            if (err.error != QJsonParseError::UnterminatedObject
                && err.error != QJsonParseError::UnterminatedString
                && err.error != QJsonParseError::UnterminatedArray) {
                qCWarning(lcBar) << "dropping unparseable socket data:" << m_buffer;
                m_buffer.clear();
            }
            break;
        }
    }
}

void BarController::handleLine(const QByteArray &line)
{
    qCDebug(lcBar) << "recv:" << line;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(lcBar) << "bad json:" << line;
        return;
    }
    processMessage(doc.object());
}

void BarController::processMessage(const QJsonObject &msg)
{
    const QString event = msg.value(QStringLiteral("event")).toString();
    const int id = msg.value(QStringLiteral("id")).toInt(-1);
    noteEvent(event + QLatin1String(" id=") + QString::number(id));

    if (event == QLatin1String("window_list")) {
        // full snapshot, sent by the compositor right after we connect
        m_windows.clear();
        const QJsonArray arr = msg.value(QStringLiteral("windows")).toArray();
        for (const QJsonValue &v : arr) {
            const QJsonObject w = v.toObject();
            const int wid = w.value(QStringLiteral("id")).toInt(-1);
            const QString appId = w.value(QStringLiteral("app_id")).toString();
            if (wid >= 0 && !appId.isEmpty())
                m_windows.addWindow(wid, appId);
        }
    } else if (event == QLatin1String("window_added") || event == QLatin1String("windows_added")) {
        const QString appId = msg.value(QStringLiteral("app_id")).toString();
        if (id >= 0 && !appId.isEmpty())
            m_windows.addWindow(id, appId);
    } else if (event == QLatin1String("window_removed") || event == QLatin1String("windows_removed")) {
        m_windows.removeWindow(id);
        if (m_focusedId == id) {
            m_focusedId = -1;
            emit focusedIdChanged();
        }
        if (m_fullscreenWindows.remove(id))
            updateFullscreenBar();
    } else if (event == QLatin1String("window_focus")) {
        // id 0 means "no window focused" in the xmonodywm protocol
        if (m_focusedId != id) {
            m_focusedId = id <= 0 ? -1 : id;
            emit focusedIdChanged();
        }
    } else if (event == QLatin1String("window_full")) {
        // the compositor sends this once when entering fullscreen and once
        // when leaving it, with no state field; track the set ourselves
        if (id > 0) {
            if (m_fullscreenWindows.contains(id))
                m_fullscreenWindows.remove(id);
            else
                m_fullscreenWindows.insert(id);
            updateFullscreenBar();
        }
    }
}

void BarController::updateFullscreenBar()
{
    setBarVisible(m_fullscreenWindows.isEmpty());
}

void BarController::noteEvent(const QString &event)
{
    if (m_lastEvent != event) {
        m_lastEvent = event;
        emit lastEventChanged();
    }
    ++m_eventsProcessed;
    emit eventsProcessedChanged();
}

void BarController::setBarVisible(bool visible)
{
    if (m_barVisible == visible)
        return;
    m_barVisible = visible;
    qCDebug(lcBar) << "bar visible:" << visible;
    emit barVisibleChanged();
}

void BarController::sendLine(const QByteArray &line)
{
    if (!m_connected)
        return;
    qCDebug(lcBar) << "send:" << line;
    m_socket.write(line);
    if (!line.endsWith('\n'))
        m_socket.write("\n");
    m_socket.flush();
}

void BarController::activateWindow(int id)
{
    for (int i = 0; i < m_windows.rowCount(); ++i) {
        const QModelIndex idx = m_windows.index(i, 0);
        if (m_windows.data(idx, WindowListModel::IdRole).toInt() == id) {
            // xmonodywm command protocol
            QJsonObject obj;
            obj.insert(QStringLiteral("action"), QStringLiteral("focus_window"));
            obj.insert(QStringLiteral("id"), id);
            sendLine(QJsonDocument(obj).toJson(QJsonDocument::Compact));
            return;
        }
    }
}

void BarController::launchApp(const QString &execLine)
{
    QString cmd = execLine.trimmed();
    if (cmd.isEmpty())
        return;
    // Desktop-spec field codes (%f, %u, ...) carry no meaning here; drop them.
    cmd.remove(QRegularExpression(QStringLiteral("%[fFuUdDnNickvm]")));
    cmd = cmd.trimmed();
    if (cmd.isEmpty())
        return;
    // Run through the shell so quoting/expansion matches the desktop file.
    if (!QProcess::startDetached(QStringLiteral("/bin/sh"), QStringList{QStringLiteral("-c"), cmd}))
        qCWarning(lcBar) << "failed to launch:" << cmd;
}

// ---------------------------------------------------------------------------
// Icon lookup
// ---------------------------------------------------------------------------

QString BarController::findIcon(const QString &appId) const
{
    return IconFinder::find(appId);
}
