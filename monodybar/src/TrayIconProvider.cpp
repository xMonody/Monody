#include "TrayIconProvider.h"

#include "TrayModel.h"

#include <QUrl>

TrayIconProvider::TrayIconProvider(TrayModel *model)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_model(model)
{
}

QImage TrayIconProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    // Strip the ?v=<rev> revision suffix, then decode the item key.
    const int q = id.indexOf(QLatin1Char('?'));
    const QString raw = q >= 0 ? id.left(q) : id;
    const QString key = QUrl::fromPercentEncoding(raw.toUtf8());
    QImage img = m_model->pixmap(key);
    if (!img.isNull() && requestedSize.isValid() && requestedSize.width() > 0
        && (img.width() > requestedSize.width() || img.height() > requestedSize.height())) {
        img = img.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (size)
        *size = img.size();
    return img;
}
