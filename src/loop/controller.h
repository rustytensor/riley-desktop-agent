// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the window (Main.qml) sees: run a task, stop, re-enable, the transcript and the current state.
//
// Each task runs in its own riley-agent-loop process (--headless, the task as --task), a child of the window; its
// output lines are the transcript. Stop and the safe word go to the child on its standard input ("stop",
// "segfault") and, at the same moment, from the window straight to riley-agentd, so a stop never waits for the
// child. Re-enable is the only place the window sends resume; the service decides it (rule R0): the window runs as
// the user, the loop does not.
#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace riley {

class Controller : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList transcript READ transcript NOTIFY transcriptChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
public:
    // loopProgram: riley-agent-loop's path; loopArguments: its options (config, socket, endpoint, limits) without
    // --headless and --task; socketPath: riley-agentd's socket, for stop, segfault and resume.
    Controller(QString loopProgram, QStringList loopArguments, QString socketPath, QObject *parent = nullptr);
    ~Controller() override;

    QStringList transcript() const { return m_transcript; }
    QString state() const { return m_state; }
    bool busy() const { return m_child != nullptr; }

    Q_INVOKABLE void run(const QString &task);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void reenable();

signals:
    void transcriptChanged();
    void stateChanged();

private:
    void add(const QString &line);
    void setState(const QString &state);
    void relay(const char *word);
    void tellService(const QString &action, const QString &label, const QString &okState);
    void readChild();
    void childEnded(const QString &state);

    QString m_loopProgram;
    QStringList m_loopArguments;
    QString m_socketPath;
    QProcess *m_child = nullptr;
    QByteArray m_pending;
    QStringList m_transcript;
    QString m_state = QStringLiteral("idle");
};

} // namespace riley
