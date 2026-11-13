// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-window-harness [riley-agent-window's own arguments]
//
// Test use only (the M4 gate, check f). Builds riley-agent-window's window the way the program does, with
// riley::createWindow (src/loop/window.h, library riley_window), shows it on the offscreen platform, waits up to
// 5 s for the item named reenableButton to be visible and enabled, presses it once with a Qt Test mouse click, lets
// the window run 3 s more, then exits. It decides nothing itself: whether the press resumes is the service's answer,
// which the gate reads from the service's log and the bar state file.
//
// Output, one line each: "pressed reenableButton" (exit 0), or "failed <what>" (exit 4); "failed not-offscreen"
// (exit 2) when QT_QPA_PLATFORM is not offscreen, so it can never show on or click the real desktop.
#include "window.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>

#include <cstdio>

namespace {

void out(const char *line)
{
    std::printf("%s\n", line);
    std::fflush(stdout);
}

QQuickItem *findItem(QQuickItem *item, const QString &name)
{
    if (!item)
        return nullptr;
    if (item->objectName() == name)
        return item;
    for (QQuickItem *child : item->childItems()) {
        if (QQuickItem *found = findItem(child, name))
            return found;
    }
    return nullptr;
}

} // namespace

int main(int argc, char **argv)
{
    if (qgetenv("QT_QPA_PLATFORM") != "offscreen") {
        out("failed not-offscreen");
        return 2;
    }
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;
    QString problem;
    QQuickWindow *window = riley::createWindow(&engine, app.arguments(), &problem);
    if (!window) {
        std::printf("failed load: %s\n", qPrintable(problem));
        return 4;
    }
    window->show();
    if (!QTest::qWaitForWindowExposed(window, 5000)) {
        out("failed not-exposed");
        return 4;
    }
    QQuickItem *button = nullptr;
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 5000) {
        button = findItem(window->contentItem(), QStringLiteral("reenableButton"));
        if (button && button->isVisible() && button->isEnabled() && button->width() > 0 && button->height() > 0)
            break;
        button = nullptr;
        QTest::qWait(50);
    }
    if (!button) {
        out("failed no-enabled-reenableButton");
        return 4;
    }
    const QPoint centre = button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre);
    out("pressed reenableButton");
    QTest::qWait(3000);
    return 0;
}
