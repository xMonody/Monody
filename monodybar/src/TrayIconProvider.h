#pragma once

#include <QQuickImageProvider>

class TrayModel;

/**
 * Serves tray icon bitmaps to QML via "image://trayicons/<key>?v=<rev>".
 * The key is the item key (service|path), percent-encoded; the revision is
 * only there to force a re-request when the icon changes.
 */
class TrayIconProvider : public QQuickImageProvider
{
public:
    explicit TrayIconProvider(TrayModel *model);

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
    TrayModel *m_model;
};
