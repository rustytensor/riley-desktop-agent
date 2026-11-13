// SPDX-License-Identifier: GPL-3.0-or-later
//
// Running the helper programs the service uses (wtype, grim, notify-send) and the apps it launches. posix_spawn
// only, so it is safe from any thread; every child is waited for by its own process ID.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace riley {

struct RunResult {
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;     // -1 when killed by a signal or not started
    QByteArray out;        // stdout
    QByteArray err;        // stderr, at most 64 KiB
};

// Runs argv[0] (an absolute path or a name looked up in PATH) with stdin from /dev/null, waits at most timeoutMs and
// kills the child (by its PID) when that runs out. env empty: the service's own environment.
RunResult runProgram(const QStringList &argv, int timeoutMs, const QStringList &env = {});

// Starts argv in a new session, in folder cwd, with exactly the environment env (KEY=VALUE), output to /dev/null.
// A detached thread waits for it so it never stays a zombie. Returns its PID, or -1.
qint64 spawnDetached(const QStringList &argv, const QString &cwd, const QStringList &env);

// The service's own environment as KEY=VALUE.
QStringList ownEnvironment();

} // namespace riley
