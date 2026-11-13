// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the window (Main.qml) sees of the loop: run a task, stop, re-enable, the transcript and the current state.
// The task field and the stop button take the same path as the headless loop's standard input: the safe word or
// stop goes to riley-agentd at once.
#pragma once

#include "agentloop.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace riley {

class Controller : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList transcript READ transcript NOTIFY transcriptChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
public:
    Controller(QString configPath, QString socketPath, QString endpointOverride, int maxSteps, int maxSeconds,
               QObject *parent = nullptr);
    ~Controller() override;

    QStringList transcript() const { return m_transcript; }
    QString state() const { return m_state; }
    bool busy() const { return m_loop && m_loop->running(); }

    Q_INVOKABLE void run(const QString &task);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void reenable();

signals:
    void transcriptChanged();
    void stateChanged();

private:
    void add(const QString &line);
    void setState(const QString &state);

    QString m_configPath;
    QString m_socketPath;
    QString m_endpointOverride;
    int m_maxSteps;
    int m_maxSeconds;
    std::unique_ptr<AgentLoop> m_loop;
    QStringList m_transcript;
    QString m_state = QStringLiteral("idle");
};

} // namespace riley
