#include "TrayItem.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QtDebug>

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

    reload();

    // Refresh our cached icon/title when the item changes them.
    QDBusConnection::sessionBus().connect(m_service, m_path,
                                          QStringLiteral("org.kde.StatusNotifierItem"),
                                          QStringLiteral("NewIcon"),
                                          this, SLOT(onNewIcon()));
    QDBusConnection::sessionBus().connect(m_service, m_path,
                                          QStringLiteral("org.kde.StatusNotifierItem"),
                                          QStringLiteral("NewTitle"),
                                          this, SLOT(onNewTitle()));
}

QVariant TrayItem::readProperty(const QString &name) const
{
    QDBusInterface iface(m_service, m_path, QStringLiteral("org.kde.StatusNotifierItem"),
                         QDBusConnection::sessionBus());
    return iface.property(name.toLatin1().constData());
}

TrayPixmapList TrayItem::readPixmaps() const
{
    // Read IconPixmap with a raw Properties.Get call instead of
    // QDBusInterface::property(): the latter caches the property signature
    // and on Qt 6.8 fails to demarshal a(iiay) for some senders (fcitx5),
    // while a manual QDBusArgument extraction always works.
    QDBusMessage call = QDBusMessage::createMethodCall(
        m_service, m_path, QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("Get"));
    call << QStringLiteral("org.kde.StatusNotifierItem")
         << QStringLiteral("IconPixmap");
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
    m_pixmaps = readPixmaps();
    m_cachedPixmap = QImage();
    m_cachedTarget = 0;
}

QImage TrayItem::iconPixmap(int target) const
{
    if (m_pixmaps.isEmpty())
        return {};
    if (m_cachedTarget == target && !m_cachedPixmap.isNull())
        return m_cachedPixmap;

    // Pick the entry closest to the target size.
    const TrayPixmapEntry *best = nullptr;
    int bestDist = INT_MAX;
    for (const TrayPixmapEntry &e : m_pixmaps) {
        const int dist = qAbs(e.width - target) + qAbs(e.height - target);
        if (dist < bestDist) {
            bestDist = dist;
            best = &e;
        }
    }
    QImage img;
    if (best && best->width > 0 && best->height > 0
        && best->data.size() >= best->width * best->height * 4) {
        // The SNI spec stores ARGB32 rows, which in memory is BGRA order -
        // exactly QImage::Format_ARGB32.
        img = QImage(reinterpret_cast<const uchar *>(best->data.constData()),
                     best->width, best->height, best->width * 4,
                     QImage::Format_ARGB32);
        img = img.copy(); // detach: data must outlive the raw QImage
    }
    if (!img.isNull() && (img.width() > target || img.height() > target))
        img = img.scaled(QSize(target, target), Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
    if (!img.isNull()) {
        m_cachedPixmap = img;
        m_cachedTarget = target;
    }
    return img;
}

void TrayItem::activate(int x, int y)
{
    QDBusInterface iface(m_service, m_path, QStringLiteral("org.kde.StatusNotifierItem"),
                         QDBusConnection::sessionBus());
    iface.call(QStringLiteral("Activate"), x, y);
}

void TrayItem::secondaryActivate(int x, int y)
{
    QDBusInterface iface(m_service, m_path, QStringLiteral("org.kde.StatusNotifierItem"),
                         QDBusConnection::sessionBus());
    iface.call(QStringLiteral("SecondaryActivate"), x, y);
}

void TrayItem::contextMenu(int x, int y)
{
    QDBusInterface iface(m_service, m_path, QStringLiteral("org.kde.StatusNotifierItem"),
                         QDBusConnection::sessionBus());
    iface.call(QStringLiteral("ContextMenu"), x, y);
}

// ---------------------------------------------------------------------------
// signal handlers
// ---------------------------------------------------------------------------

void TrayItem::onNewIcon()
{
    m_iconName = readProperty(QStringLiteral("IconName")).toString();
    m_pixmaps = readPixmaps();
    m_cachedPixmap = QImage();
    m_cachedTarget = 0;
    emit iconChanged();
}

void TrayItem::onNewTitle()
{
    m_title = readProperty(QStringLiteral("Title")).toString();
    emit titleChanged();
}
