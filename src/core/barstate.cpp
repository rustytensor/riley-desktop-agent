// SPDX-License-Identifier: GPL-3.0-or-later
#include "barstate.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace riley {

QString barActivityName(BarActivity activity)
{
    switch (activity) {
    case BarActivity::Idle: return QStringLiteral("idle");
    case BarActivity::Watching: return QStringLiteral("watching");
    case BarActivity::Acting: return QStringLiteral("acting");
    case BarActivity::Stopped: return QStringLiteral("stopped");
    }
    return QStringLiteral("idle");
}

QByteArray barStateJson(const BarState &state)
{
    QJsonObject o;
    o.insert(QStringLiteral("version"), 1);
    o.insert(QStringLiteral("state"), barActivityName(state.activity));
    o.insert(QStringLiteral("client"), state.client);
    o.insert(QStringLiteral("action"), state.activity == BarActivity::Acting ? state.action : QString());
    o.insert(QStringLiteral("since"), state.since.isValid() ? state.since.toUTC().toString(Qt::ISODateWithMs)
                                                            : QString());
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

QString defaultBarStatePath()
{
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty() || !QDir::isAbsolutePath(runtime))
        return QString();
    return runtime + QStringLiteral("/riley-desktop-agent/state.json");
}

bool writeBarState(const QString &path, const BarState &state, QString *error)
{
    if (path.isEmpty()) {
        if (error)
            *error = QStringLiteral("no state file path (XDG_RUNTIME_DIR not set)");
        return false;
    }
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir)) {
        if (error)
            *error = QStringLiteral("cannot create ") + dir;
        return false;
    }
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("cannot open ") + path + QStringLiteral(": ") + f.errorString();
        return false;
    }
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(barStateJson(state));
    if (!f.commit()) {
        if (error)
            *error = QStringLiteral("cannot write ") + path + QStringLiteral(": ") + f.errorString();
        return false;
    }
    return true;
}

} // namespace riley
