#include "IconProvider.h"

#include "IconFinder.h"

#include <QDir>
#include <QImageReader>
#include <QLibrary>
#include <QUrl>

namespace {

// ---------------------------------------------------------------------------
// gdk-pixbuf / glib, loaded at runtime. The gdk-pixbuf dev headers are not
// installed everywhere, so the handful of functions we need are declared by
// hand; the library itself is dlopen'd so the build keeps working on systems
// without gdk-pixbuf (the code then falls back to Qt's own SVG renderer).
// ---------------------------------------------------------------------------
struct GError
{
    quint32 domain;
    int code;
    char *message;
};

class GdkPixbufApi
{
public:
    static GdkPixbufApi &instance()
    {
        static GdkPixbufApi api;
        return api;
    }

    bool valid() const { return m_valid; }

    /** Render an SVG into a square (aspect-preserving) RGBA image. */
    QImage renderSvg(const QString &path, int size) const
    {
        if (!m_valid)
            return {};

        GError *err = nullptr;
        void *pb = m_newFromFileAtSize(path.toUtf8().constData(), size, size, &err);
        if (!pb) {
            if (err)
                m_clearError(&err);
            return {};
        }

        const int w = m_getWidth(pb);
        const int h = m_getHeight(pb);
        const int stride = m_getRowstride(pb);
        const uchar *pixels = m_getPixels(pb);

        // gdk-pixbuf stores straight (unpremultiplied) RGBA.
        QImage img(pixels, w, h, stride, QImage::Format_RGBA8888);
        img = img.copy(); // detach before the pixbuf is freed below
        m_objectUnref(pb);
        return img;
    }

private:
    GdkPixbufApi()
    {
        static const QLatin1String names[] = {
            QLatin1String("gdk_pixbuf-2.0"), QLatin1String("gdk-pixbuf-2.0"),
        };
        static const QLatin1String versions[] = {
            QLatin1String("0"), QLatin1String(""),
        };

        QLibrary lib;
        for (const QLatin1String &name : names) {
            for (const QLatin1String &version : versions) {
                if (version.isEmpty())
                    lib.setFileName(name);
                else
                    lib.setFileNameAndVersion(name, version);
                if (lib.load())
                    break;
            }
            if (lib.isLoaded())
                break;
        }
        if (!lib.isLoaded())
            return;

        m_newFromFileAtSize = reinterpret_cast<void *(*)(const char *, int, int, GError **)>(
            lib.resolve("gdk_pixbuf_new_from_file_at_size"));
        m_getWidth = reinterpret_cast<int (*)(void *)>(lib.resolve("gdk_pixbuf_get_width"));
        m_getHeight = reinterpret_cast<int (*)(void *)>(lib.resolve("gdk_pixbuf_get_height"));
        m_getRowstride = reinterpret_cast<int (*)(void *)>(lib.resolve("gdk_pixbuf_get_rowstride"));
        m_getPixels = reinterpret_cast<const uchar *(*)(void *)>(lib.resolve("gdk_pixbuf_get_pixels"));
        m_objectUnref = reinterpret_cast<void (*)(void *)>(lib.resolve("g_object_unref"));
        m_clearError = reinterpret_cast<void (*)(GError **)>(lib.resolve("g_clear_error"));

        m_valid = m_newFromFileAtSize && m_getWidth && m_getHeight && m_getRowstride
                  && m_getPixels && m_objectUnref && m_clearError;
    }

    void *(*m_newFromFileAtSize)(const char *, int, int, GError **) = nullptr;
    int (*m_getWidth)(void *) = nullptr;
    int (*m_getHeight)(void *) = nullptr;
    int (*m_getRowstride)(void *) = nullptr;
    const uchar *(*m_getPixels)(void *) = nullptr;
    void (*m_objectUnref)(void *) = nullptr;
    void (*m_clearError)(GError **) = nullptr;
    bool m_valid = false;
};

/** Target render size for SVG icons; clamped to something sane. */
int targetSize(const QSize &requestedSize)
{
    if (requestedSize.isValid() && requestedSize.width() > 0)
        return qBound(16, requestedSize.width(), 512);
    return 64;
}

} // namespace

IconProvider::IconProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage IconProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const QString key = QUrl::fromPercentEncoding(id.toUtf8());
    const QImage img = load(key, requestedSize);
    if (size)
        *size = img.size();
    return img;
}

QImage IconProvider::load(const QString &key, const QSize &requestedSize)
{
    const int target = targetSize(requestedSize);
    const QString cacheKey = key + QLatin1Char('@') + QString::number(target);

    const auto it = m_cache.constFind(cacheKey);
    if (it != m_cache.constEnd())
        return it.value();

    // Resolve the icon: an app_id may need the .desktop-file fallback
    // (StartupWMClass / file basename -> Icon=), absolute paths are used
    // as-is.
    const QString found = key.startsWith(QLatin1Char('/')) ? key : IconFinder::findForAppId(key);
    if (found.isEmpty())
        return {};

    // IconFinder returns file:// URLs; gdk-pixbuf needs a plain path.
    QString path = found;
    const QUrl url(found);
    if (url.isLocalFile())
        path = url.toLocalFile();

    QImage img;
    const QString lower = path.toLower();
    if (lower.endsWith(QLatin1String(".svg")) || lower.endsWith(QLatin1String(".svgz"))) {
        // librsvg (via gdk-pixbuf) handles real-world SVGs far better than
        // Qt's built-in renderer (gradient references, filters, clip paths).
        img = GdkPixbufApi::instance().renderSvg(path, target);
        if (img.isNull()) {
            QImageReader reader(path);
            reader.setScaledSize(QSize(target, target));
            img = reader.read(); // Qt's own renderer as a fallback
        }
    } else {
        img = QImage(path);
        if (!img.isNull() && (img.width() > target * 2 || img.height() > target * 2))
            img = img.scaled(QSize(target, target), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    if (!img.isNull())
        m_cache.insert(cacheKey, img);
    return img;
}
