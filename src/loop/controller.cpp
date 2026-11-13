// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller.h"

#include "safeword.h"
#include "servicecall.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QPointer>
#include <QTime>

#include <memory>

namespace riley {

Controller::Controller(QString loopProgram, QStringList loopArguments, QString socketPath, QObject *parent)
    : QObject(parent), m_loopProgram(std::move(loopProgram)), m_loopArguments(std::move(loopArguments)),
      m_socketPath(std::move(socketPath))
{
}

Controller::~Controller()
{
    if (!m_child)
        return;
    // The window closes while a task runs: the task ends with it.
    m_child->disconnect(this);
    relay("stop");
    m_child->closeWriteChannel();
    if (!m_child->waitForFinished(2000)) {
        m_child->kill();
        m_child->waitForFinished(2000);
    }
}

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

void Controller::relay(const char *word)
{
    if (m_child && m_child->state() != QProcess::NotRunning) {
        m_child->write(QByteArray(word) + '\n');
        m_child->waitForBytesWritten(100);
    }
}

void Controller::tellService(const QString &action, const QString &label, const QString &okState)
{
    // The answer never arrives while the call is being made (it comes from the event loop), so the holder is set
    // by then and the call can delete itself.
    auto holder = std::make_shared<QPointer<ServiceCall>>();
    *holder = new ServiceCall(m_socketPath, QJsonObject{{QStringLiteral("action"), action}},
                              [this, holder, action, label, okState](const ServiceReply &reply) {
        add(label + QStringLiteral(": ") + reply.line);
        if (reply.kind == QLatin1String("ok")) {
            if (!busy())
                setState(okState);
        } else if (action == QLatin1String("resume")) {
            add(QStringLiteral("the stop holds: only you can end it, here or with riley-agent resume in your own "
                               "terminal"));
        }
        if (*holder)
            (*holder)->deleteLater();
    }, this);
}

void Controller::run(const QString &task)
{
    // The safe word stops everything at once, whether a task runs or not, and asks no model.
    if (isSafeWord(task)) {
        add(QStringLiteral("segfault: stopping everything"));
        relay("segfault");
        tellService(QStringLiteral("segfault"), QStringLiteral("segfault"), QStringLiteral("stopped"));
        return;
    }
    if (busy() || task.trimmed().isEmpty())
        return;
    if (!QFileInfo(m_loopProgram).isExecutable()) {
        add(QStringLiteral("failed: no riley-agent-loop next to the window (%1)").arg(m_loopProgram));
        setState(QStringLiteral("failed"));
        return;
    }
    m_pending.clear();
    m_child = new QProcess(this);
    m_child->setProcessChannelMode(QProcess::MergedChannels);
    m_child->setWorkingDirectory(QDir::homePath());
    connect(m_child, &QProcess::readyReadStandardOutput, this, &Controller::readChild);
    connect(m_child, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readChild();
        if (!m_pending.trimmed().isEmpty())
            add(QString::fromUtf8(m_pending).trimmed());
        m_pending.clear();
        childEnded(status != QProcess::NormalExit ? QStringLiteral("failed")
                   : code == 0                    ? QStringLiteral("done")
                   : code == 3                    ? QStringLiteral("stopped")
                                                  : QStringLiteral("failed"));
    });
    connect(m_child, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        add(QStringLiteral("failed: riley-agent-loop did not start: ") + m_child->errorString());
        childEnded(QStringLiteral("failed"));
    });
    add(QStringLiteral("task: ") + task.trimmed());
    m_child->start(m_loopProgram, m_loopArguments + QStringList{QStringLiteral("--headless"), QStringLiteral("--task"),
                                                                task.trimmed()});
    setState(QStringLiteral("running"));
}

void Controller::readChild()
{
    if (!m_child)
        return;
    m_pending += m_child->readAllStandardOutput();
    qsizetype nl;
    while ((nl = m_pending.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_pending.left(nl)).trimmed();
        m_pending.remove(0, nl + 1);
        if (!line.isEmpty())
            add(line);
    }
    if (m_pending.size() > 65536) {
        add(QString::fromUtf8(m_pending.left(400)) + QStringLiteral(" ..."));
        m_pending.clear();
    }
}

void Controller::childEnded(const QString &state)
{
    if (!m_child)
        return;
    m_child->deleteLater();
    m_child = nullptr;
    setState(state);
}

void Controller::stop()
{
    relay("stop");
    tellService(QStringLiteral("stop"), QStringLiteral("stop"), QStringLiteral("stopped"));
}

// Re-enable goes to the service as resume, which decides who may end a stop (rule R0). It does not pretend: when
// the service refuses it, the window says so.
void Controller::reenable()
{
    if (busy())
        return;
    tellService(QStringLiteral("resume"), QStringLiteral("re-enable"), QStringLiteral("idle"));
}

} // namespace riley
