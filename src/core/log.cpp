// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.h"

#include <QMessageAuthenticationCode>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

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
                        const QByteArray &secret, const QDateTime &time)
{
    LogRecord r;
    r.time = time.toUTC();
    r.client = request.client;
    r.action = request.action;
    r.window = request.window;
    r.decision = decision;
    r.result = result;
    if (!request.text.isEmpty()) {
        r.textLength = request.text.toUcs4().size();   // characters (code points)
        r.textHmac = QMessageAuthenticationCode::hash(request.text.toUtf8(), secret, QCryptographicHash::Sha256)
                         .toHex();
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
        xml.writeAttribute(QStringLiteral("text-hmac"), QString::fromLatin1(r.textHmac));
    }
    xml.writeEndDocument();   // closes the empty element ("/>") and adds a newline
    while (line.endsWith(QLatin1Char('\n')))
        line.chop(1);
    return line;
}

QString controlLogLine(const QDateTime &time, const QString &client, const QString &action, const QString &result)
{
    QString line;
    QXmlStreamWriter xml(&line);
    xml.setAutoFormatting(false);
    xml.writeEmptyElement(QStringLiteral("control"));
    xml.writeAttribute(QStringLiteral("time"), time.toUTC().toString(Qt::ISODateWithMs));
    xml.writeAttribute(QStringLiteral("client"), clean(client));
    xml.writeAttribute(QStringLiteral("action"), clean(action));
    xml.writeAttribute(QStringLiteral("result"), clean(result));
    xml.writeEndDocument();
    while (line.endsWith(QLatin1Char('\n')))
        line.chop(1);
    return line;
}

QString defaultSecretPath()
{
    QString base = qEnvironmentVariable("XDG_STATE_HOME");
    if (base.isEmpty() || !QDir::isAbsolutePath(base))
        base = QDir::homePath() + QStringLiteral("/.local/state");
    return base + QStringLiteral("/riley-desktop-agent/hash-secret");
}

QByteArray loadOrCreateSecret(const QString &path, QString *error)
{
    auto fail = [error](const QString &what) {
        if (error)
            *error = what;
        return QByteArray();
    };
    if (!ensurePrivateDir(QFileInfo(path).absolutePath(), error))
        return QByteArray();
    const QByteArray native = QFile::encodeName(path);
    int fd = ::open(native.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) {
        unsigned char key[32];
        size_t got = 0;
        while (got < sizeof key) {
            const ssize_t n = getrandom(key + got, sizeof key - got, 0);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return fail(QStringLiteral("no random bytes for the hash secret"));
            got += size_t(n);
        }
        const int out = ::open(native.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (out < 0)
            return fail(QStringLiteral("cannot create ") + path);
        const bool written = ::write(out, key, sizeof key) == ssize_t(sizeof key) && ::fsync(out) == 0;
        ::close(out);
        if (!written)
            return fail(QStringLiteral("cannot write ") + path);
        fd = ::open(native.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (fd < 0)
        return fail(QStringLiteral("cannot open ") + path);
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & 077) != 0
        || st.st_size != 32) {
        ::close(fd);
        return fail(path + QStringLiteral(" must be a 32-byte file of this user with mode 0600"));
    }
    QByteArray key(32, '\0');
    const bool ok = ::read(fd, key.data(), 32) == 32;
    ::close(fd);
    if (!ok)
        return fail(QStringLiteral("cannot read ") + path);
    return key;
}

QString defaultLogPath()
{
    QString base = qEnvironmentVariable("XDG_STATE_HOME");
    if (base.isEmpty() || !QDir::isAbsolutePath(base))
        base = QDir::homePath() + QStringLiteral("/.local/state");
    return base + QStringLiteral("/riley-desktop-agent/log.xml");
}

QString defaultStopPath()
{
    QString base = qEnvironmentVariable("XDG_STATE_HOME");
    if (base.isEmpty() || !QDir::isAbsolutePath(base))
        base = QDir::homePath() + QStringLiteral("/.local/state");
    return base + QStringLiteral("/riley-desktop-agent/stopped");
}

bool appendLog(const QString &path, const LogRecord &record, QString *error)
{
    return appendLogLine(path, logLine(record), error);
}

bool appendLogLine(const QString &path, const QString &text, QString *error)
{
    if (!ensurePrivateDir(QFileInfo(path).absolutePath(), error))
        return false;
    QFile f(path);
    const bool existed = f.exists();
    if (!existed) {   // created 0600 from the start, never briefly readable by others
        const int fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd >= 0)
            ::close(fd);
    }
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error)
            *error = QStringLiteral("cannot open ") + path + QStringLiteral(": ") + f.errorString();
        return false;
    }
    if (!existed)
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QByteArray line = text.toUtf8() + '\n';
    // A last line cut off (the service killed mid-write) is ended first, so this line stays one line of its own.
    if (existed && f.size() > 0) {
        QFile last(path);
        if (last.open(QIODevice::ReadOnly) && last.seek(last.size() - 1) && last.read(1) != QByteArray("\n"))
            line.prepend('\n');
    }
    if (f.write(line) != line.size() || !f.flush()) {
        if (error)
            *error = QStringLiteral("cannot write ") + path + QStringLiteral(": ") + f.errorString();
        return false;
    }
    return true;
}

namespace {

constexpr qint64 kLogChunk = 64 * 1024;
constexpr qsizetype kLogLineMax = 1 << 20;

// One log line as written by logLine or controlLogLine: exactly one empty element, nothing else.
bool oneElement(const QByteArray &line, QString *name, QXmlStreamAttributes *attributes)
{
    QXmlStreamReader xml(line);
    int elements = 0;
    while (!xml.atEnd()) {
        switch (xml.readNext()) {
        case QXmlStreamReader::StartDocument:
        case QXmlStreamReader::EndDocument:
            break;
        case QXmlStreamReader::StartElement:
            if (++elements > 1)
                return false;
            *name = xml.name().toString();
            *attributes = xml.attributes();
            break;
        case QXmlStreamReader::EndElement:
            break;
        case QXmlStreamReader::Invalid:
            return false;
        default:   // text, comments, processing instructions: not in a line the service writes
            return false;
        }
    }
    return !xml.hasError() && elements == 1;
}

} // namespace

LogStopCheck stopFromLog(const QString &path)
{
    const QByteArray native = QFile::encodeName(path);
    const int fd = ::open(native.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT)
            return {LogStop::None, QStringLiteral("no log.xml yet")};
        return {LogStop::Unknown, QStringLiteral("log.xml cannot be opened, so a stop in it cannot be ruled out")};
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return {LogStop::Unknown, QStringLiteral("log.xml is not a regular file, so a stop in it cannot be ruled out")};
    }
    qint64 pos = st.st_size;
    QByteArray tail;          // the bytes after pos not yet split into lines
    bool last = true;         // the next line found is the file's last one
    int seen = 0;
    LogStopCheck result{LogStop::None, QStringLiteral("no stop in log.xml")};
    auto readFailed = [&] {
        result = {LogStop::Unknown, QStringLiteral("log.xml cannot be read, so a stop in it cannot be ruled out")};
    };
    // Judges one line; true when it decides (result set).
    auto judge = [&](const QByteArray &line) -> bool {
        ++seen;
        QString name;
        QXmlStreamAttributes attrs;
        if (!oneElement(line, &name, &attrs)) {
            result = {LogStop::Unknown,
                      QStringLiteral("line %1 from the end of log.xml is not one XML element, so a stop before it "
                                     "cannot be ruled out").arg(seen)};
            return true;
        }
        if (name != QLatin1String("control") || attrs.value(QLatin1String("result")) != QLatin1String("done"))
            return false;
        const QString action = attrs.value(QLatin1String("action")).toString();
        if (action == QLatin1String("stop") || action == QLatin1String("segfault")) {
            result = {LogStop::Stopped,
                      QStringLiteral("the last %1 in log.xml (%2, by %3) has no resume done after it")
                          .arg(action, attrs.value(QLatin1String("time")).toString(),
                               attrs.value(QLatin1String("client")).toString())};
            return true;
        }
        if (action == QLatin1String("resume")) {
            result = {LogStop::None, QStringLiteral("the last stop in log.xml was followed by a resume done")};
            return true;
        }
        return false;
    };
    for (;;) {
        // Split off complete lines from the end of what has been read.
        qsizetype nl;
        bool decided = false;
        while ((nl = tail.lastIndexOf('\n')) >= 0) {
            const QByteArray line = tail.mid(nl + 1);
            tail.truncate(nl);
            if (last) {
                last = false;
                if (!line.isEmpty()) {   // the file does not end with a newline: its last line was cut off
                    result = {LogStop::Unknown, QStringLiteral("the last line of log.xml is cut off, so a stop in it "
                                                               "cannot be ruled out")};
                    decided = true;
                    break;
                }
                continue;
            }
            if (!line.isEmpty() && judge(line)) {
                decided = true;
                break;
            }
        }
        if (decided)
            break;
        if (tail.size() > kLogLineMax) {
            result = {LogStop::Unknown, QStringLiteral("log.xml has a line longer than 1 MiB, so a stop before it "
                                                       "cannot be ruled out")};
            break;
        }
        if (pos == 0) {   // the first line of the file
            if (last && !tail.isEmpty())
                result = {LogStop::Unknown, QStringLiteral("the last line of log.xml is cut off, so a stop in it "
                                                           "cannot be ruled out")};
            else if (!tail.isEmpty())
                judge(tail);
            break;
        }
        const qint64 n = std::min(kLogChunk, pos);
        pos -= n;
        QByteArray chunk(n, '\0');
        qint64 got = 0;
        while (got < n) {
            const ssize_t r = ::pread(fd, chunk.data() + got, size_t(n - got), pos + got);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                break;
            got += r;
        }
        if (got != n) {
            readFailed();
            break;
        }
        tail.prepend(chunk);
    }
    ::close(fd);
    return result;
}

} // namespace riley
