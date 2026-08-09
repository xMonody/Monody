#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QSet>
#include <QTimer>

struct WindowInfo
{
    int id = -1;
    QString appId;
};

/**
 * Ordered list of windows currently shown on the taskbar.
 * Exposed to QML so a Repeater can render one icon per window.
 */
class WindowListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        AppIdRole,
    };
    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /** Add a window, keeping insertion order; updates in place when the id exists. */
    void addWindow(int id, const QString &appId);
    void removeWindow(int id);
    void clear();
    int indexOf(int id) const;

private:
    QList<WindowInfo> m_items;
};

/**
 * Talks to the compositor over a unix socket (xmonodywm).
 *
 * Incoming JSON lines (one event per line):
 *   {"event":"window_list","windows":[{"id":1,"app_id":"firefox"}],
 *    "focused_id":1}                         (snapshot on connect; focused_id is
 *                                              optional, 0/-1 = none focused)
 *   {"event":"window_added","id":1,"app_id":"firefox"}
 *   {"event":"window_removed","id":1}
 *   {"event":"window_focus","id":1}            (id 0 clears the focus)
 *   {"event":"window_full","id":1}              (sent on both enter/exit fullscreen)
 *
 * Clicking a taskbar icon sends back:
 *   {"action":"focus_window","id":1}
 *
 * Right after connecting the bar also sends:
 *   {"action":"list_windows"}    (asks for a fresh snapshot incl. focused_id
 *                                   so the focused icon shows immediately)
 */
class BarController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(WindowListModel *windows READ windows CONSTANT)
    Q_PROPERTY(int windowCount READ windowCount NOTIFY windowCountChanged)
    Q_PROPERTY(int focusedId READ focusedId NOTIFY focusedIdChanged)
    Q_PROPERTY(bool barVisible READ barVisible NOTIFY barVisibleChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool debugMode READ debugMode WRITE setDebugMode NOTIFY debugModeChanged)
    Q_PROPERTY(QString lastEvent READ lastEvent NOTIFY lastEventChanged)
    Q_PROPERTY(int eventsProcessed READ eventsProcessed NOTIFY eventsProcessedChanged)
    Q_PROPERTY(QString socketPath READ socketPath WRITE setSocketPath NOTIFY socketPathChanged)
public:
    explicit BarController(QObject *parent = nullptr);

    WindowListModel *windows() { return &m_windows; }
    int windowCount() const { return m_windows.rowCount(); }
    int focusedId() const { return m_focusedId; }
    bool barVisible() const { return m_barVisible; }
    bool connected() const { return m_connected; }
    bool debugMode() const { return m_debugMode; }
    void setDebugMode(bool on);
    QString lastEvent() const { return m_lastEvent; }
    int eventsProcessed() const { return m_eventsProcessed; }
    QString socketPath() const { return m_socketPath; }
    void setSocketPath(const QString &path);

    /** Connect to the compositor socket (retries automatically). */
    void start();

    /** Locate a themed icon for an app_id, returns a file:// URL or empty. */
    Q_INVOKABLE QString findIcon(const QString &appId) const;

    /** Send the activation JSON for the window with the given id. */
    Q_INVOKABLE void activateWindow(int id);

    /**
     * Launch a process from a .desktop Exec line (field codes are stripped,
     * quoting/expansion follows the desktop spec via /bin/sh).
     */
    Q_INVOKABLE void launchApp(const QString &execLine);

signals:
    void windowCountChanged();
    void focusedIdChanged();
    void barVisibleChanged();
    void connectedChanged();
    void debugModeChanged();
    void lastEventChanged();
    void eventsProcessedChanged();
    void socketPathChanged();

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onReadyRead();
    void tryConnect();

private:
    void handleLine(const QByteArray &line);
    void extractMessages();
    void processMessage(const QJsonObject &msg);
    void sendLine(const QByteArray &line);
    void setBarVisible(bool visible);
    void updateFullscreenBar();
    void noteEvent(const QString &event);

    WindowListModel m_windows;
    QSet<int> m_fullscreenWindows;
    int m_focusedId = -1;
    bool m_barVisible = true;
    bool m_connected = false;
    bool m_debugMode = false;
    QString m_lastEvent;
    int m_eventsProcessed = 0;
    QString m_socketPath = QStringLiteral("/tmp/xmonodywm.sock");
    QLocalSocket m_socket;
    QTimer m_reconnectTimer;
    QByteArray m_buffer;
};
