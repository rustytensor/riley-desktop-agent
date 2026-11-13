// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller.h"

#include "endpoint.h"
#include "servicecall.h"

#include <QTime>

namespace riley {

Controller::Controller(QString configPath, QString socketPath, QString endpointOverride, int maxSteps,
                       int maxSeconds, QObject *parent)
    : QObject(parent), m_configPath(std::move(configPath)), m_socketPath(std::move(socketPath)),
      m_endpointOverride(std::move(endpointOverride)), m_maxSteps(maxSteps), m_maxSeconds(maxSeconds)
{
}

Controller::~Controller() = default;

void Controller::add(const QString &line)
{
    m_transcript << QTime::currentTime().toString(QStringLiteral("HH:mm:ss ")) + line;
    while (m_transcript.size() > 500)
        m_transcript.removeFirst();
    emit transcriptChanged();
}

void Controller::setState(const QString &state)
{
    m_state = state;
    emit stateChanged();
}

void Controller::run(const QString &task)
{
    if (AgentLoop::isSafeWord(task) && busy()) {
        stop();
        return;
    }
    if (busy())
        return;
    LoopOptions options;
    options.socketPath = m_socketPath;
    options.maxSteps = m_maxSteps;
    options.maxSeconds = m_maxSeconds;
    if (!AgentLoop::isSafeWord(task)) {
        // The config is read again for every task, validated as the service does.
        const ConfigLoad load = loadConfigFile(m_configPath);
        QString why;
        if (load.status != ConfigStatus::Ok) {
            add(QStringLiteral("failed config: %1").arg(load.error.isEmpty() ? QStringLiteral("missing") : load.error));
            setState(QStringLiteral("failed"));
            return;
        }
        if (!resolveLoopEndpoint(*load.config, m_endpointOverride, &options.endpoint, &why)
            || !readEndpointKey(options.endpoint, &options.key, &why)) {
            add(QStringLiteral("failed endpoint: ") + why);
            setState(QStringLiteral("failed"));
            return;
        }
    }
    m_loop = std::make_unique<AgentLoop>(std::move(options));
    connect(m_loop.get(), &AgentLoop::step, this, &Controller::add);
    connect(m_loop.get(), &AgentLoop::finished, this, [this](int code, const QString &line) {
        add(line);
        setState(code == AgentLoop::Done ? QStringLiteral("done")
                 : code == AgentLoop::Stopped ? QStringLiteral("stopped") : QStringLiteral("failed"));
    });
    setState(QStringLiteral("running"));
    m_loop->start(task);
}

void Controller::stop()
{
    if (busy()) {
        m_loop->requestStop(QStringLiteral("stop"));
        return;
    }
    // Nothing runs here, but a stop always reaches the service.
    auto *call = new ServiceCall(m_socketPath, QJsonObject{{QStringLiteral("action"), QStringLiteral("stop")}},
                                 [this](const ServiceReply &reply) {
        add(QStringLiteral("stop: ") + reply.line);
        setState(QStringLiteral("stopped"));
    }, this);
    Q_UNUSED(call);
}

// Re-enable goes to the service as resume, which decides who may end a stop (rule R0). It does not pretend: when
// the service refuses it, the window says so and how the user ends the stop.
void Controller::reenable()
{
    if (busy())
        return;
    auto *call = new ServiceCall(m_socketPath, QJsonObject{{QStringLiteral("action"), QStringLiteral("resume")}},
                                 [this](const ServiceReply &reply) {
        add(QStringLiteral("re-enable: ") + reply.line);
        if (reply.kind == QLatin1String("ok"))
            setState(QStringLiteral("idle"));
        else
            add(QStringLiteral("to end the stop, run riley-agent resume in your own terminal"));
    }, this);
    Q_UNUSED(call);
}

} // namespace riley
