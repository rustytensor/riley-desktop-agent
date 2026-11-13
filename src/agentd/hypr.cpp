// SPDX-License-Identifier: GPL-3.0-or-later
#include "hypr.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace riley {

namespace {

QString socketPath()
{
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    const QString signature = qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE");
    if (runtime.isEmpty() || signature.isEmpty() || signature.contains(QLatin1Char('/')))
        return QString();
    return runtime + QStringLiteral("/hypr/") + signature + QStringLiteral("/.socket.sock");
}

QJsonDocument requestJson(const char *what, QString *error)
{
    const QByteArray reply = hyprRequest(QByteArray("j/") + what, error);
    if (reply.isEmpty())
        return {};
    QJsonParseError parse{};
    QJsonDocument doc = QJsonDocument::fromJson(reply, &parse);
    if (parse.error != QJsonParseError::NoError && error)
        *error = QStringLiteral("hyprland %1: %2").arg(QLatin1String(what), parse.errorString());
    return doc;
}

int intAt(const QJsonArray &a, int i)
{
    return a.size() > i ? a.at(i).toInt() : 0;
}

} // namespace

const HyprWindow *Desktop::window(const QString &address) const
{
    for (const HyprWindow &w : windows)
        if (w.address == address)
            return &w;
    return nullptr;
}

QByteArray hyprRequest(const QByteArray &request, QString *error, int timeoutMs)
{
    auto fail = [error](const QString &what) {
        if (error)
            *error = what;
        return QByteArray();
    };
    const QByteArray path = socketPath().toLocal8Bit();
    if (path.isEmpty())
        return fail(QStringLiteral("not in a Hyprland session"));
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (size_t(path.size()) >= sizeof addr.sun_path)
        return fail(QStringLiteral("Hyprland socket path too long"));
    std::memcpy(addr.sun_path, path.constData(), size_t(path.size()));
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return fail(QStringLiteral("no socket"));
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        close(fd);
        return fail(QStringLiteral("cannot connect to Hyprland"));
    }
    const char *p = request.constData();
    qsizetype left = request.size();
    while (left > 0) {
        const ssize_t n = ::send(fd, p, size_t(left), MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            return fail(QStringLiteral("cannot write to Hyprland"));
        }
        p += n;
        left -= n;
    }
    QByteArray reply;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char buffer[65536];
    for (;;) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline
                                                                               - std::chrono::steady_clock::now());
        if (ms.count() <= 0) {
            close(fd);
            return fail(QStringLiteral("Hyprland did not answer in time"));
        }
        pollfd pfd{fd, POLLIN, 0};
        const int r = poll(&pfd, 1, int(ms.count()));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            continue;
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        reply.append(buffer, n);
        if (reply.size() > 64 * 1024 * 1024)
            break;
    }
    close(fd);
    if (reply.isEmpty())
        return fail(QStringLiteral("empty answer from Hyprland"));
    return reply;
}

Desktop readDesktop()
{
    Desktop d;
    const QJsonDocument monitors = requestJson("monitors", &d.error);
    const QJsonDocument clients = requestJson("clients", &d.error);
    const QJsonDocument active = requestJson("activewindow", &d.error);
    if (!monitors.isArray() || !clients.isArray())
        return d;
    for (const QJsonValue &v : monitors.array()) {
        const QJsonObject o = v.toObject();
        HyprMonitor m;
        m.id = o.value(QStringLiteral("id")).toInt(-1);
        m.name = o.value(QStringLiteral("name")).toString();
        m.x = o.value(QStringLiteral("x")).toInt();
        m.y = o.value(QStringLiteral("y")).toInt();
        m.width = o.value(QStringLiteral("width")).toInt();
        m.height = o.value(QStringLiteral("height")).toInt();
        m.scale = o.value(QStringLiteral("scale")).toDouble(1.0);
        d.monitors.insert(m.id, m);
    }
    for (const QJsonValue &v : clients.array()) {
        const QJsonObject o = v.toObject();
        HyprWindow w;
        w.address = o.value(QStringLiteral("address")).toString();
        w.windowClass = o.value(QStringLiteral("class")).toString();
        w.title = o.value(QStringLiteral("title")).toString();
        w.pid = o.value(QStringLiteral("pid")).toInteger();
        const QJsonArray at = o.value(QStringLiteral("at")).toArray();
        const QJsonArray size = o.value(QStringLiteral("size")).toArray();
        w.x = intAt(at, 0);
        w.y = intAt(at, 1);
        w.width = intAt(size, 0);
        w.height = intAt(size, 1);
        w.monitorId = o.value(QStringLiteral("monitor")).toInt(-1);
        w.monitor = d.monitors.value(w.monitorId).name;
        w.workspaceId = o.value(QStringLiteral("workspace")).toObject().value(QStringLiteral("id")).toInt();
        w.mapped = o.value(QStringLiteral("mapped")).toBool(true);
        w.hidden = o.value(QStringLiteral("hidden")).toBool(false);
        w.floating = o.value(QStringLiteral("floating")).toBool(false);
        w.pinned = o.value(QStringLiteral("pinned")).toBool(false);
        const QJsonValue full = o.value(QStringLiteral("fullscreen"));
        w.fullscreen = full.isBool() ? full.toBool() : full.toInt() != 0;
        if (!w.address.isEmpty())
            d.windows.append(w);
    }
    if (active.isObject())
        d.activeAddress = active.object().value(QStringLiteral("address")).toString();
    d.ok = true;
    d.error.clear();
    return d;
}

QString activeWindowAddress(bool *ok)
{
    QString error;
    const QJsonDocument active = requestJson("activewindow", &error);
    if (ok)
        *ok = error.isEmpty();
    if (!active.isObject())
        return QString();
    return active.object().value(QStringLiteral("address")).toString();
}

bool hyprDispatch(const QString &expr, QString *error)
{
    const QByteArray reply = hyprRequest(QByteArray("/dispatch ") + expr.toUtf8(), error);
    if (reply.trimmed() == "ok")
        return true;
    if (error && error->isEmpty())
        *error = QString::fromUtf8(reply.trimmed().left(200));
    return false;
}

} // namespace riley
