// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamWriter>

namespace riley {

namespace {

// XML 1.0 forbids most control characters, and a newline would break the one-line format.
QString clean(const QString &value)
{
    QString out = value;
    for (QChar &c : out)
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            c = QChar(0xfffd);
    return out;
}

bool ensurePrivateDir(const QString &dir, QString *error)
{
    QDir d;
    if (!d.mkpath(dir)) {
        if (error)
            *error = QStringLiteral("cannot create ") + dir;
        return false;
    }
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return true;
}

} // namespace

LogRecord makeLogRecord(const Request &request, const Decision &decision, const QString &result,
                        const QDateTime &time)
{
    LogRecord r;
    r.time = time.toUTC();
    r.client = request.client;
    r.action = request.action;
    r.window = request.window;
    r.decision = decision;
    r.result = result;
    if (!request.text.isEmpty()) {
        r.textLength = request.text.size();
        r.textSha256 = QCryptographicHash::hash(request.text.toUtf8(), QCryptographicHash::Sha256).toHex();
    }
    return r;
}

QString logLine(const LogRecord &r)
{
    QString line;
    QXmlStreamWriter xml(&line);
    xml.setAutoFormatting(false);
    xml.writeEmptyElement(QStringLiteral("request"));
    xml.writeAttribute(QStringLiteral("time"), r.time.toUTC().toString(Qt::ISODateWithMs));
    xml.writeAttribute(QStringLiteral("client"), clean(r.client));
    xml.writeAttribute(QStringLiteral("action"), clean(r.action));
    xml.writeAttribute(QStringLiteral("class"), clean(r.window.windowClass));
    xml.writeAttribute(QStringLiteral("title"), clean(r.window.title));
    xml.writeAttribute(QStringLiteral("monitor"), clean(r.window.monitor));
    xml.writeAttribute(QStringLiteral("decision"),
                       r.decision.verdict == Verdict::Allow ? QStringLiteral("allow") : QStringLiteral("deny"));
    xml.writeAttribute(QStringLiteral("reason"), clean(r.decision.reason));
    xml.writeAttribute(QStringLiteral("rule"), clean(r.decision.rule));
    xml.writeAttribute(QStringLiteral("result"), clean(r.result));
    if (r.textLength >= 0) {
        xml.writeAttribute(QStringLiteral("text-length"), QString::number(r.textLength));
        xml.writeAttribute(QStringLiteral("text-sha256"), QString::fromLatin1(r.textSha256));
    }
    return line;
}

QString defaultLogPath()
{
    QString base = qEnvironmentVariable("XDG_STATE_HOME");
    if (base.isEmpty() || !QDir::isAbsolutePath(base))
        base = QDir::homePath() + QStringLiteral("/.local/state");
    return base + QStringLiteral("/riley-desktop-agent/log.xml");
}

bool appendLog(const QString &path, const LogRecord &record, QString *error)
{
    if (!ensurePrivateDir(QFileInfo(path).absolutePath(), error))
        return false;
    QFile f(path);
    const bool existed = f.exists();
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error)
            *error = QStringLiteral("cannot open ") + path + QStringLiteral(": ") + f.errorString();
        return false;
    }
    if (!existed)
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QByteArray line = logLine(record).toUtf8() + '\n';
    if (f.write(line) != line.size() || !f.flush()) {
        if (error)
            *error = QStringLiteral("cannot write ") + path + QStringLiteral(": ") + f.errorString();
        return false;
    }
    return true;
}

} // namespace riley
