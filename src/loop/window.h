// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-agent-window's window, as a library (riley_window) so a test harness can build exactly the same window.
//
//   riley-agent-window [--config FILE] [--socket PATH] [--endpoint NAME] [--max-steps N] [--max-seconds N]
//   riley-agent-window --list-ui
//   riley-agent-window --help
//
// The window starts riley-agent-loop (next to its own executable) for each task and relays Stop and the safe word to
// it on the child's standard input. Its Re-enable button is the only way it sends resume to riley-agentd: no option,
// input line, socket or file of the window does.
#pragma once

#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QString>
#include <QStringList>

namespace riley {

struct WindowOptions {
    bool help = false;
    bool listUi = false;
    QString config;      // empty: the default config path
    QString socket;      // empty: the default socket path
    QString endpoint;
    int maxSteps = 40;
    int maxSeconds = 600;
#ifdef RILEY_FAULT_WINDOW_HIDDEN_RESUME
    bool hiddenResume = false;   // planted fault only: an option --help does not name, which resumes at start
#endif
};

// The usage text, every option the window accepts.
QString windowUsage();

// arguments as QCoreApplication::arguments() (program name first). False with *problem set on bad usage: an option
// the window does not accept, a missing or bad value, or any other argument.
bool parseWindowArguments(const QStringList &arguments, WindowOptions *options, QString *problem);

// Parses arguments, makes the controller (owned by the engine) and loads Main.qml. Returns the window (owned by the
// engine), or nullptr with *problem set.
QQuickWindow *createWindow(QQmlApplicationEngine *engine, const QStringList &arguments, QString *problem);

} // namespace riley
