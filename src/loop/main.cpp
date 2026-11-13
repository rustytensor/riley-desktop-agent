// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-agent-loop: the agent loop, one task per run, no window (the window is riley-agent-window, which starts this
// program for each task).
//
//   riley-agent-loop --headless --task TEXT [--config FILE] [--socket PATH] [--endpoint NAME]
//                    [--max-steps N] [--max-seconds N]
//
// One line per step on standard output, the last one "done ...", "stopped ..." or "failed ..."; exit 0, 3 or 4 (2 for
// bad usage). A line "segfault" or "stop" on standard input stops everything at once.
#include "agentloop.h"
#include "config.h"
#include "endpoint.h"
#include "servicecall.h"

#include <QCoreApplication>
#include <QSocketNotifier>

#include <cerrno>
#include <cstdio>
#include <unistd.h>

using namespace riley;

namespace {

struct Args {
    bool headless = false;
    QString task;
    bool haveTask = false;
    QString config;
    QString socket;
    QString endpoint;
    int maxSteps = 40;
    int maxSeconds = 600;
};

int usage(const char *problem)
{
    std::fprintf(stderr,
                 "riley-agent-loop: %s\n"
                 "usage: riley-agent-loop --headless --task TEXT [--config FILE] [--socket PATH] [--endpoint NAME]\n"
                 "                        [--max-steps N] [--max-seconds N]\n"
                 "The window is riley-agent-window.\n",
                 problem);
    return 2;
}

bool parse(int argc, char **argv, Args *a, QString *problem)
{
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        auto value = [&](QString *out) {
            if (i + 1 >= argc) {
                *problem = arg + QStringLiteral(" needs a value");
                return false;
            }
            *out = QString::fromLocal8Bit(argv[++i]);
            return true;
        };
        auto number = [&](int *out, int max) {
            QString s;
            if (!value(&s))
                return false;
            bool ok = false;
            *out = s.toInt(&ok);
            if (!ok || *out < 1 || *out > max) {
                *problem = arg + QStringLiteral(" needs a number from 1 to %1").arg(max);
                return false;
            }
            return true;
        };
        if (arg == QLatin1String("--headless"))
            a->headless = true;
        else if (arg == QLatin1String("--task")) {
            if (!value(&a->task))
                return false;
            a->haveTask = true;
        } else if (arg == QLatin1String("--config")) {
            if (!value(&a->config))
                return false;
        } else if (arg == QLatin1String("--socket")) {
            if (!value(&a->socket))
                return false;
        } else if (arg == QLatin1String("--endpoint")) {
            if (!value(&a->endpoint))
                return false;
        } else if (arg == QLatin1String("--max-steps")) {
            if (!number(&a->maxSteps, 1000))
                return false;
        } else if (arg == QLatin1String("--max-seconds")) {
            if (!number(&a->maxSeconds, 86400))
                return false;
        } else {
            *problem = QStringLiteral("unknown argument ") + arg;
            return false;
        }
    }
    if (!a->headless) {
        *problem = QStringLiteral("--headless is needed (the window is riley-agent-window)");
        return false;
    }
    if (!a->haveTask) {
        *problem = QStringLiteral("--headless needs --task TEXT");
        return false;
    }
    if (a->config.isEmpty())
        a->config = defaultConfigPath();
    if (a->socket.isEmpty())
        a->socket = defaultSocketPath();
    return true;
}

void out(const QString &line)
{
    std::printf("%s\n", qPrintable(line));
    std::fflush(stdout);
}

int runHeadless(int argc, char **argv, const Args &a)
{
    QCoreApplication app(argc, argv);
    LoopOptions options;
    options.socketPath = a.socket;
    options.maxSteps = a.maxSteps;
    options.maxSeconds = a.maxSeconds;
    // The safe word needs no config and no model: it goes straight to the service.
    if (!AgentLoop::isSafeWord(a.task)) {
        const ConfigLoad load = loadConfigFile(a.config);
        if (load.status != ConfigStatus::Ok) {
            out(QStringLiteral("failed config: %1").arg(load.error.isEmpty() ? QStringLiteral("missing ") + a.config
                                                                             : load.error));
            return AgentLoop::Failed;
        }
        QString why;
        if (!resolveLoopEndpoint(*load.config, a.endpoint, &options.endpoint, &why)
            || !readEndpointKey(options.endpoint, &options.key, &why)) {
            out(QStringLiteral("failed endpoint: ") + why);
            return AgentLoop::Failed;
        }
    }
    AgentLoop loop(std::move(options));
    QObject::connect(&loop, &AgentLoop::step, &app, [](const QString &s) { out(s); });
    QObject::connect(&loop, &AgentLoop::finished, &app, [](int code, const QString &line) {
        out(line);
        QCoreApplication::exit(code);
    });

    // The control path: a line "segfault" or "stop" on standard input.
    QSocketNotifier control(STDIN_FILENO, QSocketNotifier::Read);
    QByteArray pending;
    QObject::connect(&control, &QSocketNotifier::activated, &app, [&] {
        char buffer[4096];
        const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof buffer);
        if (n <= 0) {
            if (n == 0 || errno != EINTR)
                control.setEnabled(false);   // no more control input; the task goes on
            return;
        }
        pending.append(buffer, n);
        qsizetype nl;
        while ((nl = pending.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(pending.left(nl)).trimmed();
            pending.remove(0, nl + 1);
            if (AgentLoop::isSafeWord(line))
                loop.requestStop(QStringLiteral("segfault"));
            else if (line.compare(QLatin1String("stop"), Qt::CaseInsensitive) == 0)
                loop.requestStop(QStringLiteral("stop"));
        }
        if (pending.size() > 65536)
            pending.clear();
    });

    QMetaObject::invokeMethod(&loop, [&loop, &a] { loop.start(a.task); }, Qt::QueuedConnection);
    return app.exec();
}

} // namespace

int main(int argc, char **argv)
{
    Args a;
    QString problem;
    if (!parse(argc, argv, &a, &problem))
        return usage(qPrintable(problem));
    return runHeadless(argc, argv, a);
}
