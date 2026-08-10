#pragma once

#include <QHash>
#include <QImage>
#include <QQuickImageProvider>

/**
 * Serves app icons to QML through "image://icons/<name-or-path>".
 *
 * The id is the app_id, the Icon= value from a .desktop file, or an absolute
 * path (percent-encoded). SVG files are rendered with librsvg through
 * gdk-pixbuf and fall back to Qt's own SVG renderer, because Qt's built-in
 * renderer silently drops parts of many real-world SVGs (Inkscape gradient
 * references, filters, clip paths, ...) - e.g. the Alacritty logo lost its
 * bright gradient parts and looked broken on the taskbar.
 *
 * Rendered icons are cached; PNG/XPM/etc. are loaded directly.
 */
class IconProvider : public QQuickImageProvider
{
public:
    explicit IconProvider();

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
    QImage load(const QString &key, const QSize &requestedSize);

    QHash<QString, QImage> m_cache;
};
