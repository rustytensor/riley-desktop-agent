// SPDX-License-Identifier: GPL-3.0-or-later
//
// One request to riley-agentd over its socket, without blocking: the same JSON line riley-agent sends, and the
// service's answer (kind, first line, listing lines, and for an ok see the PNG as base64).
#pragma once

#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace riley {

struct ServiceReply {
    QString kind;     // ok, refused, stopped or failed
    QString line;     // the service's first line, as riley-agent prints it
    QStringList body; // listing lines (windows, tree)
    QString png;      // base64 PNG, only for an ok see
};

class ServiceCall : public QObject
{
    Q_OBJECT
public:
    using Done = std::function<void(const ServiceReply &)>;

    // Starts at once. done is called exactly once, unless the call is abandoned first.
    ServiceCall(const QString &socketPath, const QJsonObject &request, Done done, QObject *parent = nullptr);
    ~ServiceCall() override;

    // Drop the answer: done is never called, the connection is closed.
    void abandon();

private:
    void finish(const ServiceReply &reply);
    void parse();

    QLocalSocket m_socket;
    QByteArray m_request;
    QByteArray m_buffer;
    Done m_done;
    bool m_finished = false;
};

// $XDG_RUNTIME_DIR/riley-desktop-agent/agentd.sock, or empty without XDG_RUNTIME_DIR.
QString defaultSocketPath();

} // namespace riley
