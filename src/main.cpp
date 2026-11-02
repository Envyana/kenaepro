#include "core/Session.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QPalette>
#include <QStyleFactory>
#include <QTimer>

#include <cstdio>

namespace {

// A dark theme, like other review tools: the picture is the brightest thing on screen. Fusion looks
// the same on every desktop, so the AppImage does not depend on the system's widget style.
void applyDarkTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette palette;
    const QColor window(45, 45, 48);
    const QColor base(30, 30, 32);
    const QColor text(220, 220, 220);
    const QColor accent(52, 132, 228);
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, window);
    palette.setColor(QPalette::ToolTipBase, QColor(60, 60, 64));
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::PlaceholderText, QColor(130, 130, 130));
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, QColor(58, 58, 62));
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Light, QColor(80, 80, 84));
    palette.setColor(QPalette::Midlight, QColor(66, 66, 70));
    palette.setColor(QPalette::Mid, QColor(40, 40, 42));
    palette.setColor(QPalette::Dark, QColor(25, 25, 27));
    palette.setColor(QPalette::Shadow, Qt::black);
    palette.setColor(QPalette::Highlight, accent);
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, accent.lighter(130));
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        palette.setColor(QPalette::Disabled, role, QColor(115, 115, 115));
    }
    palette.setColor(QPalette::Disabled, QPalette::Button, QColor(50, 50, 53));
    app.setPalette(palette);
    app.setStyleSheet(QStringLiteral(
        "QToolTip { border: 1px solid #555; padding: 4px; }"
        "QDockWidget::title { padding: 4px 6px; background: #38383c; }"
        "QStatusBar { border-top: 1px solid #222; }"));
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("KenaePRO"));
    QApplication::setApplicationVersion(QStringLiteral(FR_VERSION));  // project(VERSION) in CMakeLists.txt
    applyDarkTheme(app);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Frame-by-frame video review and annotation"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption apiPortOption(QStringLiteral("api-port"),
                                           QStringLiteral("Enable the local Client API on <port> (127.0.0.1)."),
                                           QStringLiteral("port"));
    parser.addOption(apiPortOption);
    // Test aids: show a frame, save a picture of the window, and exit.
    const QCommandLineOption frameOption(QStringLiteral("frame"),
                                         QStringLiteral("Seek to <n> (0-based) after opening the file."),
                                         QStringLiteral("n"));
    const QCommandLineOption screenshotOption(QStringLiteral("screenshot"),
                                              QStringLiteral("Save a picture of the window to <png> and exit."),
                                              QStringLiteral("png"));
    parser.addOption(frameOption);
    parser.addOption(screenshotOption);
    parser.addPositionalArgument(QStringLiteral("file"),
                                 QStringLiteral("Project (.kpro) or media file to open."),
                                 QStringLiteral("[file]"));
    parser.process(app);

    fr::Session session;
    fr::MainWindow window(session);

    // --api-port wins over the preference; without either the API stays off.
    if (parser.isSet(apiPortOption)) {
        bool ok = false;
        const quint16 port = parser.value(apiPortOption).toUShort(&ok);
        if (!ok || port == 0) {
            std::fprintf(stderr, "kenaepro: --api-port needs a port number from 1 to 65535\n");
            return 2;
        }
        QString error;
        if (!window.startApi(port, &error)) {
            std::fprintf(stderr, "kenaepro: cannot listen on port %u: %s\n", port,
                         qPrintable(error));
            return 1;
        }
    } else if (window.preferences().apiEnabled()) {
        QString error;
        if (!window.startApi(window.preferences().apiPort(), &error)) {
            std::fprintf(stderr, "kenaepro: Client API from the preferences is off: %s\n",
                         qPrintable(error));
        }
    }

    window.show();
    const QStringList files = parser.positionalArguments();
    if (!files.isEmpty()) window.openPath(files.first());
    if (parser.isSet(frameOption)) session.playback().seek(parser.value(frameOption).toInt());

    if (parser.isSet(screenshotOption)) {
        const QString path = parser.value(screenshotOption);
        QTimer::singleShot(1000, &app, [&app, &window, path] {
            const bool saved = window.grab().save(path);
            app.exit(saved ? 0 : 3);
        });
    }
    return app.exec();
}
