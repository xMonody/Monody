#include "TrayItem.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusVariant>
#include <QtDebug>
#include <functional>

namespace {

// How long an icon-change blink (QQ fallback) lasts before it auto-stops.
constexpr int kIconChangeBlinkMs = 10000;
// Icon changes are ignored this long after a user click (the app swaps its
// icon back to normal right after the click - that must not blink again).
constexpr int kIconChangeCooldownMs = 5000;

/** Decode one SNI pixmap (network-order ARGB32) into an RGBA8888 QImage. */
QImage decodePixmap(const TrayPixmapList &list, int target)
{
    if (list.isEmpty())
        return {};

    // Pick the entry closest to the target size.
    const TrayPixmapEntry *best = nullptr;
    int bestDist = INT_MAX;
    for (const TrayPixmapEntry &e : list) {
        const int dist = qAbs(e.width - target) + qAbs(e.height - target);
        if (dist < bestDist) {
            bestDist = dist;
            best = &e;
        }
    }

    QImage img;
    if (best && best->width > 0 && best->height > 0
        && best->data.size() >= best->width * best->height * 4) {
        // The SNI spec stores IconPixmap as ARGB32 in network byte order
        // (big-endian): each pixel is the four bytes A,R,G,B. QImage has no
        // matching in-memory format - Format_ARGB32 is host-endian
        // 0xAARRGGBB, i.e. B,G,R,A bytes on little-endian - so interpreting
        // the data directly swaps alpha with one colour channel and makes
        // the icon look washed-out/grey (what QQ's tray icon did). Copy the
        // bytes into RGBA8888 (R,G,B,A) explicitly instead.
        const int w = best->width;
        const int h = best->height;
        img = QImage(w, h, QImage::Format_RGBA8888);
        const uchar *src = reinterpret_cast<const uchar *>(best->data.constData());
        for (int y = 0; y < h; ++y) {
            uchar *dst = img.scanLine(y);
            const uchar *row = src + y * w * 4;
            for (int x = 0; x < w; ++x) {
                dst[x * 4 + 0] = row[x * 4 + 1]; // R
                dst[x * 4 + 1] = row[x * 4 + 2]; // G
                dst[x * 4 + 2] = row[x * 4 + 3]; // B
                dst[x * 4 + 3] = row[x * 4 + 0]; // A
            }
        }
    }
    if (!img.isNull() && (img.width() > target || img.height() > target))
        img = img.scaled(QSize(target, target), Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
    return img;
}

/** Byte-exact comparison of two images (QImage has no operator==). */
bool sameImage(const QImage &a, const QImage &b)
{
    if (a.size() != b.size())
        return false;
    const QImage aa = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage bb = b.convertToFormat(QImage::Format_RGBA8888);
    return aa.sizeInBytes() == bb.sizeInBytes()
        && memcmp(aa.constBits(), bb.constBits(), aa.sizeInBytes()) == 0;
}

/** Recursively parse one (ia{sv}av) node of a dbusmenu layout. */
void parseMenuNode(const QDBusArgument &arg, MenuItem &item)
{
    arg.beginStructure();
    arg >> item.id;
    QVariantMap props;
    arg >> props;
    item.label = props.value(QStringLiteral("label")).toString();
    item.enabled = props.value(QStringLiteral("enabled"), true).toBool();
    item.visible = props.value(QStringLiteral("visible"), true).toBool();
    item.type = props.value(QStringLiteral("type")).toString();
    item.toggleType = props.value(QStringLiteral("toggle-type")).toString();
    QVariantList children;
    arg >> children; // av: each element is another (ia{sv}av)
    for (const QVariant &c : std::as_const(children)) {
        if (c.canConvert<QDBusArgument>()) {
            MenuItem child;
            parseMenuNode(c.value<QDBusArgument>(), child);
            item.children.append(child);
        }
    }
    arg.endStructure();
}

} // namespace

QDBusArgument &operator<<(QDBusArgument &arg, const TrayPixmapEntry &entry)
{
    arg.beginStructure();
    arg << entry.width << entry.height << entry.data;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, TrayPixmapEntry &entry)
{
    arg.beginStructure();
    arg >> entry.width >> entry.height >> entry.data;
    arg.endStructure();
    return arg;
}

TrayItem::TrayItem(const QString &service, const QString &path, QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_path(path)
{
    static const bool registered = [] {
        qDBusRegisterMetaType<TrayPixmapEntry>();
        qDBusRegisterMetaType<TrayPixmapList>();
        return true;
    }();
    Q_UNUSED(registered)

    m_attentionTimer.setSingleShot(true);
    connect(&m_attentionTimer, &QTimer::timeout, this, [this] {
        if (m_attentionSource != AttentionSource::Signal)
            setAttention(false, AttentionSource::None);
    });
    m_cooldownTimer.setSingleShot(true);
    connect(&m_cooldownTimer, &QTimer::timeout, this, [this] {
        m_iconChangeCooldown = false;
    });

    reload();

    // Remember the owning process so notifications (Notify calls) from this
    // app can be matched back to its tray item.
    const QDBusReply<uint> pidReply =
        QDBusConnection::sessionBus().interface()->servicePid(m_service);
    if (pidReply.isValid())
        m_pid = pidReply.value();

    // Refresh our cached icon/title/attention when the item changes them.
    QDBusConnection::sessionBus().connect(m_service, m_path,
                                          QStringLiteral("org.kde.StatusNotifierItem"),
                                          QStringLiteral("NewIcon"),
                                          this, SLOT(onNewIcon()));
    QDBusConnection::sessionBus().connect(m_service, m_path,
                                          QStringLiteral("org.kde.StatusNotifierItem"),
                                          QStringLiteral("NewTitle"),
                                          this, SLOT(onNewTitle()));
    QDBusConnection::sessionBus().connect(m_service, m_path,
                                          QStringLiteral("org.kde.StatusNotifierItem"),
                                          QStringLiteral("NewAttentionIcon"),
                                          this, SLOT(onNewAttentionIcon()));
    QDBusConnection::sessionBus().connect(m_service, m_path,
                                          QStringLiteral("org.kde.StatusNotifierItem"),
                                          QStringLiteral("NewStatus"),
                                          this, SLOT(onNewStatus()));
}

QVariant TrayItem::readProperty(const QString &name) const
{
    QDBusInterface iface(m_service, m_path, QStringLiteral("org.kde.StatusNotifierItem"),
                         QDBusConnection::sessionBus());
    return iface.property(name.toLatin1().constData());
}

TrayPixmapList TrayItem::readPixmaps(const QString &property) const
{
    // Read the pixmap property with a raw Properties.Get call instead of
    // QDBusInterface::property(): the latter caches the property signature
    // and on Qt 6.8 fails to demarshal a(iiay) for some senders (fcitx5),
    // while a manual QDBusArgument extraction always works.
    QDBusMessage call = QDBusMessage::createMethodCall(
        m_service, m_path, QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("Get"));
    call << QStringLiteral("org.kde.StatusNotifierItem") << property;
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return {};

    // The reply's argument is a variant wrapping the pixmap list; unwrap it.
    const QVariant inner = reply.arguments().at(0).value<QDBusVariant>().variant();
    if (inner.canConvert<TrayPixmapList>())
        return inner.value<TrayPixmapList>();
    if (inner.canConvert<QDBusArgument>()) {
        QDBusArgument arg = inner.value<QDBusArgument>();
        TrayPixmapList list;
        arg >> list;
        return list;
    }
    return {};
}

void TrayItem::reload()
{
    m_title = readProperty(QStringLiteral("Title")).toString();
    m_iconName = readProperty(QStringLiteral("IconName")).toString();
    m_pixmaps = readPixmaps(QStringLiteral("IconPixmap"));
    m_attentionIconName = readProperty(QStringLiteral("AttentionIconName")).toString();
    m_attentionPixmaps = readPixmaps(QStringLiteral("AttentionIconPixmap"));
    m_status = readProperty(QStringLiteral("Status")).toString();
    // The Menu property is a dbus ObjectPath (fcitx5) or a plain string
    // (QQ): accept both.
    const QVariant menuV = readProperty(QStringLiteral("Menu"));
    if (menuV.canConvert<QDBusObjectPath>())
        m_menuPath = menuV.value<QDBusObjectPath>().path();
    else
        m_menuPath = menuV.toString();
    m_cachedPixmap = QImage();
    m_cachedTarget = 0;
    m_cachedAttentionPixmap = QImage();
    m_cachedAttentionTarget = 0;
    m_attentionActive = (m_status == QLatin1String("NeedsAttention"));
    m_attentionSource = m_attentionActive ? AttentionSource::Signal : AttentionSource::None;
    m_lastIcon = iconPixmap(24); // baseline for icon-change detection
}

QImage TrayItem::iconPixmap(int target) const
{
    if (m_pixmaps.isEmpty())
        return {};
    if (m_cachedTarget == target && !m_cachedPixmap.isNull())
        return m_cachedPixmap;

    QImage img = decodePixmap(m_pixmaps, target);
    if (!img.isNull()) {
        m_cachedPixmap = img;
        m_cachedTarget = target;
    }
    return img;
}

QImage TrayItem::attentionIconPixmap(int target) const
{
    if (m_attentionPixmaps.isEmpty())
        return {};
    if (m_cachedAttentionTarget == target && !m_cachedAttentionPixmap.isNull())
        return m_cachedAttentionPixmap;

    QImage img = decodePixmap(m_attentionPixmaps, target);
    if (!img.isNull()) {
        m_cachedAttentionPixmap = img;
        m_cachedAttentionTarget = target;
    }
    return img;
}

void TrayItem::activate(int x, int y)
{
    onUserInteraction();
    QDBusInterface iface(m_service, m_path, QStringLiteral("org.kde.StatusNotifierItem"),
                         QDBusConnection::sessionBus());
    iface.call(QStringLiteral("Activate"), x, y);
}

void TrayItem::secondaryActivate(int x, int y)
{
    onUserInteraction();
    QDBusInterface iface(m_service, m_path, QStringLiteral("org.kde.StatusNotifierItem"),
                         QDBusConnection::sessionBus());
    iface.call(QStringLiteral("SecondaryActivate"), x, y);
}

// ---------------------------------------------------------------------------
// dbusmenu (SNI Menu property): the app's own context menu
// ---------------------------------------------------------------------------

bool TrayItem::fetchMenu()
{
    m_menuItems.clear();
    if (m_menuPath.isEmpty())
        return false;

    // GetLayout(parentId=0, recursionDepth=2, properties) - depth 2 fetches
    // the whole tree including one submenu level.
    QDBusMessage call = QDBusMessage::createMethodCall(
        m_service, m_menuPath, QStringLiteral("com.canonical.dbusmenu"),
        QStringLiteral("GetLayout"));
    call << 0 << 2
         << QStringList{ QStringLiteral("label"), QStringLiteral("enabled"),
                         QStringLiteral("visible"), QStringLiteral("type"),
                         QStringLiteral("children-display"),
                         QStringLiteral("toggle-type") };
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2)
        return false;
    const QVariant layout = reply.arguments().at(1);
    if (!layout.canConvert<QDBusArgument>())
        return false;
    MenuItem root;
    parseMenuNode(layout.value<QDBusArgument>(), root);
    m_menuItems = root.children;
    return true;
}

QList<MenuItem> TrayItem::menuChildren(int parentId) const
{
    if (parentId == 0)
        return m_menuItems;
    std::function<QList<MenuItem>(const QList<MenuItem> &)> find =
        [&](const QList<MenuItem> &list) -> QList<MenuItem> {
            for (const MenuItem &mi : list) {
                if (mi.id == parentId)
                    return mi.children;
                if (!mi.children.isEmpty()) {
                    const QList<MenuItem> sub = find(mi.children);
                    if (!sub.isEmpty())
                        return sub;
                }
            }
            return {};
        };
    return find(m_menuItems);
}

void TrayItem::triggerMenuItem(int id)
{
    if (m_menuPath.isEmpty())
        return;
    QDBusMessage call = QDBusMessage::createMethodCall(
        m_service, m_menuPath, QStringLiteral("com.canonical.dbusmenu"),
        QStringLiteral("Event"));
    // data is a variant (v); a plain QVariant(QString()) marshals as 's'
    // and an empty QDBusVariant fails to marshal.  A QDBusVariant wrapping a
    // non-empty value marshals as 'v' with that value inside.
    call << id << QStringLiteral("clicked")
         << QVariant::fromValue(QDBusVariant(QVariant(QString()))) << quint32(0);
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call);
    if (reply.type() == QDBusMessage::ErrorMessage)
        qWarning() << "[menu] event error:" << reply.errorMessage();
}

// ---------------------------------------------------------------------------
// attention helpers
// ---------------------------------------------------------------------------

void TrayItem::flashAttention()
{
    // Blink until the user interacts with the item (a click opens the app's
    // main window) or the app recovers - no auto-stop, QQ does not signal
    // "message read" in any other way.
    setAttention(true, AttentionSource::Notification);
    m_attentionTimer.stop();
}

void TrayItem::setAttention(bool on, AttentionSource source)
{
    m_attentionSource = on ? source : AttentionSource::None;
    if (m_attentionActive == on)
        return;
    m_attentionActive = on;
    emit attentionChanged();
}

void TrayItem::onUserInteraction()
{
    if (m_attentionActive) {
        setAttention(false, AttentionSource::None);
        emit iconChanged(); // icon URL reverts to the normal one
    }
    m_attentionTimer.stop();
    // The app (QQ) swaps its icon back to normal right after a click; keep
    // the icon-change blink quiet for a while so "message read" doesn't blink.
    m_iconChangeCooldown = true;
    m_cooldownTimer.start(kIconChangeCooldownMs);
}

// ---------------------------------------------------------------------------
// signal handlers
// ---------------------------------------------------------------------------

void TrayItem::onNewIcon()
{
    m_iconName = readProperty(QStringLiteral("IconName")).toString();
    m_pixmaps = readPixmaps(QStringLiteral("IconPixmap"));
    m_cachedPixmap = QImage();
    m_cachedTarget = 0;

    const QImage now = iconPixmap(24);
    if (m_attentionSource == AttentionSource::Signal) {
        // The app recovered from a real SNI attention state: emitting NewIcon
        // is how it switches back to its normal icon.
        setAttention(false, AttentionSource::None);
        m_attentionTimer.stop();
        m_lastIcon = now;
    } else if (m_attentionSource == AttentionSource::IconChange
               || m_attentionSource == AttentionSource::Notification) {
        // Already blinking because the icon changed / a notification arrived
        // (QQ-style): keep blinking, just refresh the baseline.  Only the
        // icon-change fallback auto-stops (timer); notification blinks stay
        // until the user interacts.
        if (!sameImage(m_lastIcon, now)) {
            m_lastIcon = now;
            if (m_attentionSource == AttentionSource::IconChange)
                m_attentionTimer.start(kIconChangeBlinkMs);
        }
    } else if (!m_iconChangeCooldown && !m_lastIcon.isNull()
               && !sameImage(m_lastIcon, now)) {
        // QQ-style apps don't emit NewAttentionIcon: a message just swaps the
        // icon. Blink when the icon visibly changes.
        m_lastIcon = now;
        setAttention(true, AttentionSource::IconChange);
        m_attentionTimer.start(kIconChangeBlinkMs);
    } else {
        m_lastIcon = now;
    }

    emit iconChanged();
    emit attentionChanged();
}

void TrayItem::onNewTitle()
{
    m_title = readProperty(QStringLiteral("Title")).toString();
    if (m_attentionSource == AttentionSource::Signal) {
        setAttention(false, AttentionSource::None);
        m_attentionTimer.stop();
    }
    emit titleChanged();
    emit attentionChanged();
}

void TrayItem::onNewAttentionIcon()
{
    m_attentionIconName = readProperty(QStringLiteral("AttentionIconName")).toString();
    m_attentionPixmaps = readPixmaps(QStringLiteral("AttentionIconPixmap"));
    m_cachedAttentionPixmap = QImage();
    m_cachedAttentionTarget = 0;
    setAttention(true, AttentionSource::Signal);
    m_attentionTimer.stop();
    emit iconChanged(); // the icon URL changes to the attention one
}

void TrayItem::onNewStatus()
{
    m_status = readProperty(QStringLiteral("Status")).toString();
    if (m_status == QLatin1String("NeedsAttention")) {
        setAttention(true, AttentionSource::Signal);
        m_attentionTimer.stop();
    } else if (m_attentionSource == AttentionSource::Signal) {
        setAttention(false, AttentionSource::None);
        m_attentionTimer.stop();
    }
    emit attentionChanged();
}
