#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>

#include "BarController.h"
#include "LayerShellQt/shell.h"
#include "LayerShellQt/window.h"

// Must match `height` in qml/main.qml (also used as the exclusive zone).
constexpr int BAR_HEIGHT = 48;

// The xmonodywm compositor puts its IPC socket in $XDG_RUNTIME_DIR and only
// falls back to /tmp when that variable is unset. Mirror it so the bar finds
// the socket automatically.
static QString defaultSocketPath()
{
    const QByteArray rt = qgetenv("XDG_RUNTIME_DIR");
    if (!rt.isEmpty())
        return QString::fromLocal8Bit(rt) + QLatin1String("/xmonodywm.sock");
    return QStringLiteral("/tmp/xmonodywm.sock");
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qt6-bar"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Qt6 floating taskbar using wlr-layer-shell"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption socketOption(QStringLiteral("socket"),
                                    QStringLiteral("Compositor socket path (default: $XDG_RUNTIME_DIR/xmonodywm.sock)"),
                                    QStringLiteral("path"),
                                    defaultSocketPath());
    parser.addOption(socketOption);
    parser.process(app);

    // Ask Qt Wayland to use the layer-shell shell integration for this app.
    LayerShellQt::Shell::useLayerShell();

    BarController controller;
    controller.setSocketPath(parser.value(socketOption));
    controller.setDebugMode(qEnvironmentVariableIsSet("BAR_DEBUG"));

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("bar"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("barHeight"), BAR_HEIGHT);
    engine.load(QUrl(QStringLiteral("qrc:/qml/main.qml")));
    if (engine.rootObjects().isEmpty())
        return -1;

    // Turn the taskbar window into a top-anchored layer-shell surface.
    if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) {
        if (auto *shell = LayerShellQt::Window::get(window)) {
            shell->setLayer(LayerShellQt::Window::LayerTop);
            shell->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop)
                              | LayerShellQt::Window::AnchorLeft
                              | LayerShellQt::Window::AnchorRight);
            shell->setExclusiveZone(BAR_HEIGHT);
            shell->setMargins(QMargins(0, 0, 0, 0));
            shell->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        }

        // "window_full" hides the bar; the reverse shows it again.
        QObject::connect(&controller, &BarController::barVisibleChanged, window, [&controller, window]() {
            window->setVisible(controller.barVisible());
        });
        window->setVisible(controller.barVisible());
    }

    controller.start();

    return app.exec();
}
