#pragma once

#include <QDBusArgument>
#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

// One entry of the StatusNotifierItem IconPixmap property (a(iiay)).
struct TrayPixmapEntry
{
    int width = 0;
    int height = 0;
    QByteArray data; // ARGB32, network byte order (A,R,G,B), width*height*4 bytes
};
using TrayPixmapList = QList<TrayPixmapEntry>;
Q_DECLARE_METATYPE(TrayPixmapEntry)
Q_DECLARE_METATYPE(TrayPixmapList)

QDBusArgument &operator<<(QDBusArgument &arg, const TrayPixmapEntry &entry);
const QDBusArgument &operator>>(const QDBusArgument &arg, TrayPixmapEntry &entry);

// One node of the app's com.canonical.dbusmenu menu (from the SNI Menu
// property).  Children are the submenu entries.
struct MenuItem
{
    int id = 0;
    QString label;
    bool enabled = true;
    bool visible = true;
    QString type;      // "", "separator", "checkbox", "radio"
    QString toggleType;
    QList<MenuItem> children;
};

/**
 * Client for one StatusNotifierItem (a system-tray icon exported by an app
 * over DBus: fcitx5, QQ, WeChat, Discord, ...).
 *
 * Besides the plain icon/title it tracks the "attention" state used for
 * message notifications:
 *  - SNI apps (WeChat, Telegram, ...) emit NewAttentionIcon and/or set
 *    Status to "NeedsAttention" and provide AttentionIconPixmap/Name; the
 *    panel shows the attention icon blinking until the app recovers (it
 *    emits NewIcon/NewTitle or changes Status again).
 *  - Electron apps (QQ) never emit NewAttentionIcon - a message just swaps
 *    the regular icon via NewIcon.  As a fallback the icon blinks when its
 *    content visibly changes; a user click stops the blink and starts a
 *    short cooldown so the app swapping its icon back does not blink again.
 */
class TrayItem : public QObject
{
    Q_OBJECT
public:
    TrayItem(const QString &service, const QString &path, QObject *parent = nullptr);

    /** Unique key identifying this item (service + object path). */
    QString key() const { return m_service + QLatin1Char('|') + m_path; }

    QString title() const { return m_title; }
    QString iconName() const { return m_iconName; }

    /** True when the item provided a bitmap (preferred over IconName). */
    bool hasPixmap() const { return !m_pixmaps.isEmpty(); }
    /** Best icon bitmap (closest to target), scaled to fit. */
    QImage iconPixmap(int target = 24) const;

    // -- attention (message notification) ---------------------------------
    /** True while the item wants attention (the tray icon should blink). */
    bool attentionActive() const { return m_attentionActive; }
    bool hasAttentionPixmap() const { return !m_attentionPixmaps.isEmpty(); }
    QString attentionIconName() const { return m_attentionIconName; }
    QImage attentionIconPixmap(int target = 24) const;

    /** Pid of the process behind this item (for notification matching). */
    quint64 pid() const { return m_pid; }

    /** Name of the process behind this item (read from /proc/<pid>/comm). */
    QString processName() const { return m_processName; }

    /** True when the app exports a com.canonical.dbusmenu (SNI Menu). */
    bool hasMenu() const { return !m_menuPath.isEmpty(); }
    /** Load the menu tree via GetLayout(0, 2, ...); true on success. */
    bool fetchMenu();
    /** Direct children of parentId (0 = top level). */
    QList<MenuItem> menuChildren(int parentId) const;
    /**
     * Send a "clicked" Event for a menu item.  Returns false when the item
     * failed to trigger (e.g. its id was stale after the app rebuilt the
     * menu).
     */
    bool triggerMenuItem(int id);

    /**
     * Blink the icon for a while because a notification was received from
     * this item's process (QQ-style apps only notify via Notify, never via
     * StatusNotifierItem signals).  Stops on user interaction.
     */
    void flashAttention();

    /** Stop blinking without sending the SNI Activate call - used when the
     *  app window is activated from the taskbar instead of its tray icon. */
    void dismissNotification();

    /** Notify the item of user interaction (coordinates are in logical px). */
    void activate(int x, int y);
    void secondaryActivate(int x, int y);

signals:
    void iconChanged();
    void titleChanged();
    void attentionChanged();

private slots:
    void onNewIcon();
    void onNewTitle();
    void onNewAttentionIcon();
    void onNewStatus();

private:
    enum class AttentionSource { None, Signal, IconChange, Notification };

    QVariant readProperty(const QString &name) const;
    TrayPixmapList readPixmaps(const QString &property) const;
    void reload();
    void setAttention(bool on, AttentionSource source);
    /** A click on the icon: stop blinking, quiet icon-change blinks briefly. */
    void onUserInteraction();

    // dbusmenu helpers (QQ rebuilds its menu ids frequently, so a cached id
    // may be stale when the user clicks: map by label path and retry)
    bool sendMenuEvent(int id, const QString &eventId);
    QStringList labelPathForId(int id) const;
    int idForLabelPath(const QStringList &path) const;

    QString m_service;
    QString m_path;
    QString m_title;
    QString m_iconName;
    TrayPixmapList m_pixmaps;
    mutable QImage m_cachedPixmap;
    mutable int m_cachedTarget = 0;
    quint64 m_pid = 0;
    QString m_processName;       // basename of the owning process (for sorting)
    QString m_menuPath;          // SNI Menu property (dbusmenu object path)
    QList<MenuItem> m_menuItems; // cached menu tree (top level)

    // attention state
    QString m_attentionIconName;
    TrayPixmapList m_attentionPixmaps;
    mutable QImage m_cachedAttentionPixmap;
    mutable int m_cachedAttentionTarget = 0;
    QString m_status;
    bool m_attentionActive = false;
    AttentionSource m_attentionSource = AttentionSource::None;

    // QQ-style icon-change blink fallback
    QImage m_lastIcon;                 // last seen (24 px) icon
    bool m_iconChangeCooldown = false; // suppress blinks right after a click
    QTimer m_attentionTimer;           // auto-stops an icon-change blink
    QTimer m_cooldownTimer;
};
