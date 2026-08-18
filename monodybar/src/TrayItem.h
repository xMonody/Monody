#pragma once

#include <QDBusArgument>
#include <QImage>
#include <QObject>
#include <QString>

// One entry of the StatusNotifierItem IconPixmap property (a(iiay)).
struct TrayPixmapEntry
{
    int width = 0;
    int height = 0;
    QByteArray data; // ARGB32 (BGRA byte order), width*height*4 bytes
};
using TrayPixmapList = QList<TrayPixmapEntry>;
Q_DECLARE_METATYPE(TrayPixmapEntry)
Q_DECLARE_METATYPE(TrayPixmapList)

QDBusArgument &operator<<(QDBusArgument &arg, const TrayPixmapEntry &entry);
const QDBusArgument &operator>>(const QDBusArgument &arg, TrayPixmapEntry &entry);

/**
 * Client for one StatusNotifierItem (a system-tray icon exported by an app
 * over DBus: fcitx5, QQ, WeChat, Discord, ...).  Reads the icon/title, tracks
 * the NewIcon/NewTitle signals and forwards user actions back to the item.
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

    /** Notify the item of user interaction (coordinates are in logical px). */
    void activate(int x, int y);
    void secondaryActivate(int x, int y);
    void contextMenu(int x, int y);

signals:
    void iconChanged();
    void titleChanged();

private slots:
    void onNewIcon();
    void onNewTitle();

private:
    QVariant readProperty(const QString &name) const;
    TrayPixmapList readPixmaps() const;
    void reload();

    QString m_service;
    QString m_path;
    QString m_title;
    QString m_iconName;
    TrayPixmapList m_pixmaps;
    mutable QImage m_cachedPixmap;
    mutable int m_cachedTarget = 0;
};
