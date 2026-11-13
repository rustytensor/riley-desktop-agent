// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-agent-window: the desktop agent's window (see window.h). Exit 0, or 2 for bad usage, 4 when the window
// cannot be built.
#include "window.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>

#include <cstdio>

using namespace riley;

namespace {

void out(const QString &line)
{
    std::printf("%s\n", qPrintable(line));
    std::fflush(stdout);
}

void listItems(QQuickItem *item)
{
    if (!item)
        return;
    if (!item->objectName().isEmpty()) {
        QString type = QString::fromLatin1(item->metaObject()->className());
        type.remove(QRegularExpression(QStringLiteral("_QML(TYPE)?_[0-9]+$")));
        if (type.startsWith(QLatin1String("QQuick")))
            type = type.mid(6);
        out(item->objectName() + QLatin1Char(' ') + type);
    }
    for (QQuickItem *child : item->childItems())
        listItems(child);
}

} // namespace

int main(int argc, char **argv)
{
    // Every argument is checked before Qt sees any: Qt's own options (-platform, -qmljsdebugger, ...) are bad usage.
    QStringList arguments;
    for (int i = 0; i < argc; ++i)
        arguments << QString::fromLocal8Bit(argv[i]);
    WindowOptions options;
    QString problem;
    if (!parseWindowArguments(arguments, &options, &problem)) {
        std::fprintf(stderr, "riley-agent-window: %s\n%s", qPrintable(problem), qPrintable(windowUsage()));
        return 2;
    }
    if (options.help) {
        std::printf("%s", qPrintable(windowUsage()));
        return 0;
    }
    if (options.listUi && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");   // --list-ui never shows anything

    int qtArgc = 1;   // Qt gets the program name only
    QGuiApplication app(qtArgc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("riley-agent-window"));
    // The window's class on Hyprland: riley-desktop-agent, which the service never lets any client see or act on
    // (built-in refusal R6b).
    QGuiApplication::setDesktopFileName(QStringLiteral("riley-desktop-agent"));
    QQmlApplicationEngine engine;
    QQuickWindow *window = createWindow(&engine, arguments, &problem);
    if (!window) {
        std::fprintf(stderr, "riley-agent-window: %s\n", qPrintable(problem));
        return 4;
    }
    if (options.listUi) {
        out(window->objectName() + QStringLiteral(" ApplicationWindow"));
        listItems(window->contentItem());
        return 0;
    }
    return app.exec();
}
