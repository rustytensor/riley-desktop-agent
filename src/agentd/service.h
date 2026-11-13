// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-agentd's request handling: one request per connection on its Unix socket (the protocol is in
// docs/files.md). Every request is identified (SO_PEERCRED and process ancestry), filled with the target window's
// facts, decided by the permission engine, acted on only when allowed, and logged.
#pragma once

#include "barstate.h"
#include "config.h"
#include "hypr.h"
#include "policy.h"
#include "uinput.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace riley {

struct ServiceOptions {
    QString configPath, socketPath, statePath, logPath, secretPath;
    QString stopPath;     // the stop file (docs/files.md): a stop survives a restart of the service
    QString ownLoopExe;   // the tool's own agent loop executable, for recognising it as a caller
};

class Service
{
public:
    Service(ServiceOptions options, QByteArray secret);

    void setConfig(const ConfigLoad &load);
    bool openMouse(QString *error) { return m_mouse.open(error); }
    void closeMouse() { m_mouse.close(); }

    // Handles one connection to the end and closes fd. Runs on its own thread.
    void serve(int fd);

    // Ends sessions that have been quiet for a while (called every few seconds from the main thread).
    void tick();

    // Writes the bar state at rest: stopped while a stop is in force, else idle (used at start and at exit).
    void writeRestState();

    // Stop or segfault: revoke everything, end the action in progress.
    void stop(const QString &client, const QString &word);
    void resume(const QString &client);

private:
    struct Reply {
        QString kind;        // ok, refused, stopped, failed
        QString line;        // the first line the command line prints
        QStringList body;    // further lines
        QByteArray png;      // see: the picture
    };
    struct Job;   // one allowed action in progress

    Reply handleAction(int fd, const QString &client, const QString &action, const QJsonObject &request);
    Reply runAllowed(Job &job);
    Reply doType(Job &job);
    Reply doKey(Job &job);
    Reply doClick(Job &job);
    Reply doPoint(Job &job);
    Reply doSee(Job &job);
    Reply doTree(Job &job);
    Reply doLaunch(Job &job);
    Reply doClose(Job &job);
    Reply doWindows(const QString &client, const Desktop &desktop, quint64 generation);

    bool moveCursor(Job &job, int gx, int gy, QString *error);
    // A key event, or refused/stopped: the per-key re-check before every key event.
    bool recheckBeforeKey(Job &job, Reply *reply, int sent);

    ConfigLoad config() const;
    WindowInfo windowInfo(const HyprWindow &w, const Desktop &desktop) const;
    bool stoppedSince(quint64 generation) const { return m_stopGeneration.load() != generation; }
    bool waitMs(int ms, quint64 generation);   // false when stopped meanwhile
    void beginActing(const QString &client, const QString &action);
    void endActing(const QString &client);
    void writeBarLocked();
    bool writeStopFile(const QString &client, QString *error);
    void log(const Request &request, const Decision &decision, const QString &result);
    void logControl(const QString &client, const QString &action, const QString &result);

    ServiceOptions m_options;
    QByteArray m_secret;
    VirtualMouse m_mouse;

    mutable std::mutex m_configMutex;
    ConfigLoad m_config;

    std::mutex m_stateMutex;   // bar and sessions; m_stopped changes only under it
    std::atomic<bool> m_stopped{false};
    BarState m_bar;
    QHash<QString, qint64> m_sessions;   // client kind -> last request, ms since the epoch

    std::mutex m_stopMutex;   // with m_stopCondition: wakes waits between key events at once on stop
    std::condition_variable m_stopCondition;
    std::atomic<quint64> m_stopGeneration{0};

    std::timed_mutex m_actionMutex;   // one action at a time; stop and resume never wait for it
    std::mutex m_logMutex;
};

} // namespace riley
