// SPDX-License-Identifier: GPL-3.0-or-later
#include "procs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>

namespace riley {

namespace {

struct ProcInfo {
    QString comm;
    qint64 ppid = -1;
};

bool readProc(qint64 pid, ProcInfo *info)
{
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly))
        return false;
    const QByteArray text = stat.readAll();
    const qsizetype open = text.indexOf('(');
    const qsizetype close = text.lastIndexOf(')');
    if (open < 0 || close < open)
        return false;
    info->comm = QString::fromUtf8(text.mid(open + 1, close - open - 1));
    const QList<QByteArray> rest = text.mid(close + 2).split(' ');
    if (rest.size() < 2)
        return false;
    bool ok = false;
    info->ppid = rest.at(1).toLongLong(&ok);
    return ok;
}

QString agentForComm(const QString &comm)
{
    if (comm == QLatin1String("claude"))
        return QStringLiteral("claude-code");
    if (comm == QLatin1String("pi"))
        return QStringLiteral("pi");
    if (comm == QLatin1String("codex"))
        return QStringLiteral("codex");
    return QString();
}

} // namespace

QString callerKind(qint64 pid, const QString &ownLoopExe)
{
    qint64 current = pid;
    for (int depth = 0; depth < 128 && current > 1; ++depth) {
        ProcInfo info;
        if (!readProc(current, &info))
            return QString();
        if (!ownLoopExe.isEmpty()) {
            const QString exe = QFileInfo(QStringLiteral("/proc/%1/exe").arg(current)).symLinkTarget();
            if (!exe.isEmpty() && exe == ownLoopExe)
                return QStringLiteral("own-loop");
        }
        const QString agent = agentForComm(info.comm);
        if (!agent.isEmpty())
            return agent;
        current = info.ppid;
    }
    return QString();
}

QString windowAgent(qint64 pid)
{
    if (pid <= 1)
        return QString();
    // One pass over /proc for the parent of every process, then a walk down from the window's process.
    QHash<qint64, QList<qint64>> children;
    QHash<qint64, QString> comms;
    const QStringList entries = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &e : entries) {
        bool ok = false;
        const qint64 p = e.toLongLong(&ok);
        if (!ok)
            continue;
        ProcInfo info;
        if (!readProc(p, &info))
            continue;
        children[info.ppid].append(p);
        comms.insert(p, info.comm);
    }
    QList<qint64> queue{pid};
    for (qsizetype i = 0; i < queue.size() && i < 4096; ++i) {
        const qint64 p = queue.at(i);
        const QString agent = agentForComm(comms.value(p));
        if (!agent.isEmpty())
            return agent;
        queue.append(children.value(p));
    }
    return QString();
}

} // namespace riley
