// SPDX-License-Identifier: GPL-3.0-or-later
#include "servicecall.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace riley {

ServiceCall::ServiceCall(const QString &socketPath, const QJsonObject &request, Done done, QObject *parent)
    : QObject(parent), m_request(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n'), m_done(std::move(done))
{
    connect(&m_socket, &QLocalSocket::connected, this, [this] { m_socket.write(m_request); });
    connect(&m_socket, &QLocalSocket::readyRead, this, [this] { m_buffer += m_socket.readAll(); });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        m_buffer += m_socket.readAll();
        parse();
    });
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError error) {
        if (error == QLocalSocket::PeerClosedError)
            return;   // the service closes after its answer: handled by disconnected
        if (m_buffer.isEmpty())
            finish({QStringLiteral("failed"),
                    QStringLiteral("failed no-service: riley-agentd is not running (%1)").arg(m_socket.serverName()),
                    {}, {}});
    });
    if (socketPath.isEmpty()) {
        QMetaObject::invokeMethod(this, [this] {
            finish({QStringLiteral("failed"), QStringLiteral("failed no-service: no socket path (XDG_RUNTIME_DIR not "
                                                             "set?)"), {}, {}});
        }, Qt::QueuedConnection);
        return;
    }
    m_socket.connectToServer(socketPath);
}

ServiceCall::~ServiceCall()
{
    m_finished = true;
    m_socket.abort();
}

void ServiceCall::abandon()
{
    m_finished = true;
    m_socket.abort();
}

void ServiceCall::parse()
{
    const QJsonObject answer = QJsonDocument::fromJson(m_buffer).object();
    m_buffer.clear();
    ServiceReply reply;
    reply.kind = answer.value(QStringLiteral("kind")).toString();
    reply.line = answer.value(QStringLiteral("line")).toString();
    for (const QJsonValue &v : answer.value(QStringLiteral("body")).toArray())
        reply.body << v.toString();
    reply.png = answer.value(QStringLiteral("png")).toString();
    if (reply.kind.isEmpty() || reply.line.isEmpty())
        reply = {QStringLiteral("failed"), QStringLiteral("failed no-answer: riley-agentd closed the connection"), {},
                 {}};
    finish(reply);
}

void ServiceCall::finish(const ServiceReply &reply)
{
    if (m_finished)
        return;
    m_finished = true;
    if (m_done)
        m_done(reply);
}

QString defaultSocketPath()
{
    const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        return QString();
    return QString::fromLocal8Bit(runtime) + QStringLiteral("/riley-desktop-agent/agentd.sock");
}

} // namespace riley
