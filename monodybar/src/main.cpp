#include <QCommandLineParser>
#include <QDBusMetaType>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>

#include "BarConfig.h"
#include "BarController.h"
#include "BatteryModule.h"
#include "BluetoothModule.h"
#include "DesktopApps.h"
#include "IconIndex.h"
#include "IconProvider.h"
#include "NetworkModule.h"
#include "NotificationDaemon.h"
#include "PowerModule.h"
#include "TrayIconProvider.h"
#include "TrayItem.h"
#include "TrayModel.h"
#include "TrayWatcher.h"
#include "VolumeModule.h"
#include "LayerShellQt/window.h"

// Must match `height` in qml/main.qml (also used as the exclusive zone).
// Height comes from barCfg::height so the bar and its exclusive zone stay in sync.

// The monodywm compositor puts its IPC socket in $XDG_RUNTIME_DIR and only
// falls back to /tmp when that variable is unset. Mirror it so the bar finds
// the socket automatically.
static QString defaultSocketPath()
{
    const QByteArray rt = qgetenv("XDG_RUNTIME_DIR");
    if (!rt.isEmpty())
        return QString::fromLocal8Bit(rt) + QLatin1String("/monodywm.sock");
    return QStringLiteral("/tmp/monodywm.sock");
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("monodybar"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));

    // 自定义字体 (barCfg::font, 见 BarConfig.h): 支持系统字体家族名,
    // 也支持字体文件路径 (.ttf/.otf, 自动加载并用其中的第一个家族)。
    // 通过 QGuiApplication::setFont 设为全局默认, 所有 QML 文本继承
    // (显式指定了 font.family 的除外, 如调试面板)。
    {
        QString fontFamily = barCfg::font;
        if (!fontFamily.isEmpty()) {
            if (QFile::exists(fontFamily)) {
                const int id = QFontDatabase::addApplicationFont(fontFamily);
                if (id >= 0) {
                    const QStringList families =
                            QFontDatabase::applicationFontFamilies(id);
                    if (!families.isEmpty())
                        fontFamily = families.first();
                }
            }
            QFont f = app.font();
            f.setFamily(fontFamily);
            app.setFont(f);
        }
    }

    // Register the SNI IconPixmap type (a(iiay)) up front so every DBus
    // property read can demarshal it without warning.
    qDBusRegisterMetaType<TrayPixmapEntry>();
    qDBusRegisterMetaType<TrayPixmapList>();

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Qt6 floating taskbar using wlr-layer-shell"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption socketOption(QStringLiteral("socket"),
                                    QStringLiteral("Compositor socket path (default: $XDG_RUNTIME_DIR/monodywm.sock)"),
                                    QStringLiteral("path"),
                                    defaultSocketPath());
    parser.addOption(socketOption);
    parser.process(app);

    // Layer-shell is the Qt Wayland shell integration since Qt 6.5; no need
    // to call LayerShellQt::Shell::useLayerShell() anymore (deprecated).

    BarController controller;
    controller.setSocketPath(parser.value(socketOption));
    controller.setDebugMode(qEnvironmentVariableIsSet("BAR_DEBUG"));

    // Build the one-time icon index (XDG .desktop files + icon themes) before
    // anything draws: afterwards every icon lookup is a plain hash lookup.
    IconIndex::init();

    DesktopAppsModel desktopApps;

    // System tray (StatusNotifier host): apps like fcitx5, QQ and WeChat
    // register their tray icon here.
    TrayWatcher trayWatcher;
    TrayModel trayModel;
    trayModel.setWatcher(&trayWatcher);
    trayWatcher.registerService();
    // Pick up tray items that were exported before the watcher existed
    // (apps started before the bar may never re-register).
    trayWatcher.discoverExistingItems();

    // Freedesktop notification daemon: apps like QQ announce incoming
    // messages only through Notify (never via SNI signals), so owning the
    // name lets us flash their tray icon on a message.
    NotificationDaemon notificationDaemon;
    trayWatcher.connectNotificationDaemon(&notificationDaemon);
    notificationDaemon.registerService();

    // When the user activates an app window from the taskbar (or the window
    // gets focused), stop the matching tray icon from blinking.
    QObject::connect(&controller, &BarController::windowActivated,
                     &trayWatcher, &TrayWatcher::clearAttentionForPid);

    // System-bus status modules (battery / network / bluetooth): each one
    // watches its D-Bus service and exposes state to QML (see module/).
    // The volume module talks to PipeWire natively (module/VolumeModule.h),
    // driven by Qt's event loop via QSocketNotifier.
    BatteryModule batteryModule;
    NetworkModule networkModule;
    BluetoothModule bluetoothModule;
    VolumeModule volumeModule;
    PowerModule powerModule;

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("icons"), new IconProvider);
    engine.addImageProvider(QStringLiteral("trayicons"), new TrayIconProvider(&trayModel));
    engine.rootContext()->setContextProperty(QStringLiteral("bar"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("trayModel"), &trayModel);
    engine.rootContext()->setContextProperty(QStringLiteral("batteryModule"), &batteryModule);
    engine.rootContext()->setContextProperty(QStringLiteral("networkModule"), &networkModule);
    engine.rootContext()->setContextProperty(QStringLiteral("bluetoothModule"), &bluetoothModule);
    engine.rootContext()->setContextProperty(QStringLiteral("volumeModule"), &volumeModule);
    engine.rootContext()->setContextProperty(QStringLiteral("powerModule"), &powerModule);
    engine.rootContext()->setContextProperty(QStringLiteral("barHeight"), barCfg::height);
    engine.rootContext()->setContextProperty(QStringLiteral("desktopApps"), &desktopApps);

    // Appearance settings from BarConfig.h (see also the anchor block below)
    engine.rootContext()->setContextProperty(QStringLiteral("barCfgAtTop"), barCfg::atTop);
    engine.rootContext()->setContextProperty(QStringLiteral("barCfgWidth"), barCfg::width);
    engine.rootContext()->setContextProperty(QStringLiteral("barCfgRadius"), barCfg::radius);
    engine.rootContext()->setContextProperty(QStringLiteral("barCfgOpacity"), barCfg::opacity);
    engine.rootContext()->setContextProperty(QStringLiteral("barCfgBarColor"), barCfg::barColor);
    engine.rootContext()->setContextProperty(QStringLiteral("barCfgActiveBg"), barCfg::activeBg);
    engine.rootContext()->setContextProperty(QStringLiteral("barCfgMenuBg"), barCfg::menuBg);
    engine.rootContext()->setContextProperty(QStringLiteral("barCfgActiveBorder"), barCfg::activeBorder);
    engine.rootContext()->setContextProperty(QStringLiteral("barCfgWinAppGap"), barCfg::winAppGap);
    engine.rootContext()->setContextProperty(QStringLiteral("barCfgAppGap"), barCfg::appGap);
    engine.rootContext()->setContextProperty(QStringLiteral("barCfgFocusPad"), barCfg::focusPad);
    engine.rootContext()->setContextProperty(QStringLiteral("barCfgFocusRadius"), barCfg::focusRadius);
    engine.rootContext()->setContextProperty(QStringLiteral("barCfgShowStatusPercent"), barCfg::showStatusPercent);
    engine.rootContext()->setContextProperty(QStringLiteral("barCfgStatusGap"), barCfg::statusGap);
    engine.load(QUrl(QStringLiteral("qrc:/qml/main.qml")));
    if (engine.rootObjects().isEmpty())
        return -1;

    // Turn the taskbar window into a top-anchored layer-shell surface.
    if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) {
        if (auto *shell = LayerShellQt::Window::get(window)) {
            shell->setLayer(LayerShellQt::Window::LayerTop);
            // Full-width bar: stretch across the whole output edge.
            LayerShellQt::Window::Anchors anchors;
            if (barCfg::width <= 0)
                anchors = LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorLeft)
                          | LayerShellQt::Window::AnchorRight;
            // Fixed-width bar: no horizontal anchor, so the compositor
            // centres the surface on the output.
            anchors |= barCfg::atTop ? LayerShellQt::Window::AnchorTop
                                     : LayerShellQt::Window::AnchorBottom;
            shell->setAnchors(anchors);
            shell->setExclusiveZone(barCfg::height);
            shell->setMargins(QMargins(0, 0, 0, 0));
            shell->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        }

        // "window_full" hides the bar; the reverse shows it again.
        QObject::connect(&controller, &BarController::barVisibleChanged, window, [&controller, window]() {
            window->setVisible(controller.barVisible());
        });
        window->setVisible(controller.barVisible());
    }

    // The launcher (start menu) is a full-screen layer surface on top of
    // everything: it owns the whole screen while open, so any click outside
    // the panel (handled in QML) closes it reliably, no xdg_popup / focus
    // semantics required. The panel itself is placed under the win icon in QML.
    if (auto *launcher = engine.rootObjects().first()->findChild<QQuickWindow *>(QStringLiteral("launcherWindow"))) {
        if (auto *shell = LayerShellQt::Window::get(launcher)) {
            shell->setLayer(LayerShellQt::Window::LayerOverlay);
            shell->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop
                                                            | LayerShellQt::Window::AnchorBottom
                                                            | LayerShellQt::Window::AnchorLeft
                                                            | LayerShellQt::Window::AnchorRight));
            shell->setMargins(QMargins(0, 0, 0, 0));
            shell->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
            shell->setExclusiveZone(0);
            shell->setScope(QStringLiteral("monodybar-launcher"));
        }
    } else {
        qWarning() << "launcher window not found";
    }

    // Popup overlays: full-screen layer surfaces on top of everything. Each
    // owns the whole screen while open so any click outside the panel
    // (handled in QML) closes it, and each can render outside the bar's
    // small surface.  Every popup has its own layer-shell scope.
    const struct {
        const char *objectName;
        const char *scope;
    } popupWindows[] = {
        { "contextMenuWindow",   "monodybar-context-menu"   },
        { "trayMenuWindow",      "monodybar-tray-menu"      },
        { "quickSettingsWindow", "monodybar-quick-settings" },
    };
    for (const auto &popup : popupWindows) {
        if (auto *popupWin = engine.rootObjects().first()->findChild<QQuickWindow *>(QString::fromLatin1(popup.objectName))) {
            if (auto *shell = LayerShellQt::Window::get(popupWin)) {
                shell->setLayer(LayerShellQt::Window::LayerOverlay);
                shell->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop
                                                                | LayerShellQt::Window::AnchorBottom
                                                                | LayerShellQt::Window::AnchorLeft
                                                                | LayerShellQt::Window::AnchorRight));
                shell->setMargins(QMargins(0, 0, 0, 0));
                shell->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
                shell->setExclusiveZone(0);
                shell->setScope(QString::fromLatin1(popup.scope));
            }
        } else {
            qWarning() << popup.objectName << "not found";
        }
    }

    controller.start();

    return app.exec();
}
