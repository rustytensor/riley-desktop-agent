// SPDX-License-Identifier: GPL-3.0-or-later
#include "endpoint.h"

#include <QFile>
#include <QUrl>

namespace riley {

bool resolveLoopEndpoint(const Config &config, const QString &override, Endpoint *out, QString *why)
{
    const Client *client = config.client(QStringLiteral("own-loop"));
    if (!client) {
        *why = QStringLiteral("the config lists no own-loop client");
        return false;
    }
    const QString name = override.isEmpty() ? client->endpoint : override;
    if (name.isEmpty()) {
        *why = QStringLiteral("the own-loop client names no endpoint (give one with --endpoint NAME)");
        return false;
    }
    const Endpoint *endpoint = config.endpoint(name);
    if (!endpoint) {
        *why = QStringLiteral("the config has no endpoint named %1").arg(name);
        return false;
    }
    if (endpoint->location == Location::Remote && !isRemoteClient(config, QStringLiteral("own-loop"))) {
        *why = QStringLiteral("endpoint %1 is remote, but the service counts the loop as local; name it in the "
                              "own-loop client so the remote allow-list applies").arg(name);
        return false;
    }
    const QUrl url(endpoint->baseUrl);
    if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))
        || url.host().isEmpty()) {
        *why = QStringLiteral("endpoint %1 has no usable http(s) base-url").arg(name);
        return false;
    }
    *out = *endpoint;
    return true;
}

bool readEndpointKey(const Endpoint &endpoint, QByteArray *key, QString *why)
{
    key->clear();
    if (!endpoint.keyEnv.isEmpty()) {
        *key = qgetenv(endpoint.keyEnv.toLocal8Bit().constData()).trimmed();
        if (key->isEmpty()) {
            *why = QStringLiteral("the key variable %1 is not set").arg(endpoint.keyEnv);
            return false;
        }
    } else if (!endpoint.keyFile.isEmpty()) {
        QFile file(endpoint.keyFile);
        if (!file.open(QIODevice::ReadOnly)) {
            *why = QStringLiteral("cannot read the key file %1").arg(endpoint.keyFile);
            return false;
        }
        *key = file.read(16384).trimmed();
        if (key->isEmpty()) {
            *why = QStringLiteral("the key file %1 is empty").arg(endpoint.keyFile);
            return false;
        }
    } else {
        return true;
    }
    for (const char c : std::as_const(*key)) {
        if (static_cast<unsigned char>(c) < 0x21 || static_cast<unsigned char>(c) > 0x7e) {
            key->fill('\0');
            key->clear();
            *why = QStringLiteral("the key of endpoint %1 is not one word of printable characters").arg(endpoint.name);
            return false;
        }
    }
    return true;
}

} // namespace riley
