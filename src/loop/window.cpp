// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"

#include "config.h"
#include "controller.h"
#include "servicecall.h"

#include <QCoreApplication>
#include <QDir>
#include <QtQml/QQmlExtensionPlugin>

Q_IMPORT_QML_PLUGIN(Riley_LoopPlugin)

namespace riley {

QString windowUsage()
{
    return QStringLiteral(
        "usage: riley-agent-window [--config FILE] [--socket PATH] [--endpoint NAME] [--max-steps N]\n"
        "                          [--max-seconds N]\n"
        "       riley-agent-window --list-ui\n"
        "       riley-agent-window --help\n"
        "\n"
        "The desktop agent's window: give the agent a task and watch it. Each task runs in riley-agent-loop.\n"
        "  --config FILE      config.xml (default ~/.config/riley-desktop-agent/config.xml)\n"
        "  --socket PATH      riley-agentd's socket (default $XDG_RUNTIME_DIR/riley-desktop-agent/agentd.sock)\n"
        "  --endpoint NAME    another endpoint of the config for the loop\n"
        "  --max-steps N      at most N model requests per task (default 40)\n"
        "  --max-seconds N    at most N seconds per task (default 600)\n"
        "  --list-ui          build the window offscreen, print its named items, exit\n"
        "  --help             this text\n"
        "Stop, or the task segfault, stops everything at once. Re-enable ends a stop.\n");
}

bool parseWindowArguments(const QStringList &arguments, WindowOptions *o, QString *problem)
{
    *o = WindowOptions{};
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString &arg = arguments.at(i);
        auto value = [&](QString *out) {
            if (i + 1 >= arguments.size()) {
                *problem = arg + QStringLiteral(" needs a value");
                return false;
            }
            *out = arguments.at(++i);
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
        if (arg == QLatin1String("--help") || arg == QLatin1String("-h")) {
            o->help = true;
        } else if (arg == QLatin1String("--list-ui")) {
            o->listUi = true;
        } else if (arg == QLatin1String("--config")) {
            if (!value(&o->config))
                return false;
        } else if (arg == QLatin1String("--socket")) {
            if (!value(&o->socket))
                return false;
        } else if (arg == QLatin1String("--endpoint")) {
            if (!value(&o->endpoint))
                return false;
        } else if (arg == QLatin1String("--max-steps")) {
            if (!number(&o->maxSteps, 1000))
                return false;
        } else if (arg == QLatin1String("--max-seconds")) {
            if (!number(&o->maxSeconds, 86400))
                return false;
        } else {
            *problem = QStringLiteral("unknown argument ") + arg;
            return false;
        }
    }
    return true;
}

QQuickWindow *createWindow(QQmlApplicationEngine *engine, const QStringList &arguments, QString *problem)
{
    WindowOptions o;
    if (!parseWindowArguments(arguments, &o, problem))
        return nullptr;

    // The loop runs next to the window: the same build or the same install, never one found on PATH.
    const QString loop = QCoreApplication::applicationDirPath() + QStringLiteral("/riley-agent-loop");
    QStringList loopArgs{QStringLiteral("--config"), o.config.isEmpty() ? defaultConfigPath() : o.config,
                         QStringLiteral("--socket"), o.socket.isEmpty() ? defaultSocketPath() : o.socket,
                         QStringLiteral("--max-steps"), QString::number(o.maxSteps),
                         QStringLiteral("--max-seconds"), QString::number(o.maxSeconds)};
    if (!o.endpoint.isEmpty())
        loopArgs << QStringLiteral("--endpoint") << o.endpoint;

    auto *controller = new Controller(loop, loopArgs, o.socket.isEmpty() ? defaultSocketPath() : o.socket, engine);
    engine->setInitialProperties({{QStringLiteral("controller"), QVariant::fromValue<QObject *>(controller)}});
    engine->loadFromModule("Riley.Loop", "Main");
    if (engine->rootObjects().isEmpty()) {
        *problem = QStringLiteral("the window's QML did not load");
        return nullptr;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine->rootObjects().constFirst());
    if (!window)
        *problem = QStringLiteral("the window's QML has no window");
    return window;
}

} // namespace riley
