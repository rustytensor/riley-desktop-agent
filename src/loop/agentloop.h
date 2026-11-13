// SPDX-License-Identifier: GPL-3.0-or-later
//
// The agent loop: a task in, then ask the model, act through riley-agentd, report back, repeat, until the model
// answers without a tool call, calls done, a limit is reached, or a stop comes.
//
// - Model requests: POST <base-url>/chat/completions, not streamed, OpenAI tool calling. The key goes only in the
//   Authorization header; it is never printed, logged or put in the transcript.
// - Every tool call is one request to the service, which decides and logs it. Its answer goes back to the model as
//   a tool message that starts with the service's first line, so refusals reach the model with their reason.
// - Pictures: only for a see the service answered ok, as it gave them, in one user message after that reply's tool
//   messages. Earlier pictures are dropped from the history when a new one comes.
// - Stop: requestStop("segfault" or "stop") sends that word to the service (which ends any action in progress and
//   revokes everything), abandons the model request in flight, and ends the task; no model request follows.
#pragma once

#include "config.h"
#include "servicecall.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QNetworkReply;

namespace riley {

struct LoopOptions {
    QString socketPath;
    Endpoint endpoint;
    QByteArray key;          // empty: no Authorization header
    int maxSteps = 40;       // model requests per task
    int maxSeconds = 600;    // wall time per task
};

class AgentLoop : public QObject
{
    Q_OBJECT
public:
    enum Exit { Done = 0, Stopped = 3, Failed = 4 };

    explicit AgentLoop(LoopOptions options, QObject *parent = nullptr);
    ~AgentLoop() override;

    // The safe word as the task stops everything and asks no model.
    void start(const QString &task);
    void requestStop(const QString &word);
    bool running() const { return m_running; }

    static bool isSafeWord(const QString &text);

signals:
    // One step for the transcript or standard output: never the key, never picture data.
    void step(const QString &text);
    // The end: exit code (Done, Stopped, Failed) and the last line ("done ...", "stopped ...", "failed ...").
    void finished(int code, const QString &line);

private:
    struct Call {
        QString id;
        QString name;
        QJsonObject args;
        QString argError;
    };
    struct Picture {
        QString callId;
        QString window;
        QString png;   // base64, as the service gave it
    };

    void askModel();
    void onModelReply(QNetworkReply *reply);
    void runNext();
    void onToolReply(const Call &call, const ServiceReply &reply);
    void addPictures();
    void dropOldPictures();
    void finish(int code, const QString &line);

    static QJsonArray toolDefinitions();
    static QString systemPrompt();
    static bool serviceRequest(const Call &call, QJsonObject *request, QString *why);
    static QString describe(const Call &call);

    LoopOptions m_options;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QPointer<ServiceCall> m_call;
    QPointer<ServiceCall> m_control;
    QJsonArray m_messages;
    QList<Call> m_queue;
    QList<Picture> m_pictures;   // from this reply's sees, sent after its tool messages
#ifdef RILEY_FAULT_LOOP_IMAGE_LEAK
    Picture m_lastPicture;       // planted fault (gate builds only): a cached picture, re-sent after a refused see
#endif
    QTimer m_deadline;
    int m_steps = 0;
    bool m_running = false;
    bool m_stopping = false;
};

} // namespace riley
