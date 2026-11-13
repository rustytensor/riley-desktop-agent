// SPDX-License-Identifier: GPL-3.0-or-later
#include "service.h"

#include "a11y.h"
#include "log.h"
#include "procs.h"
#include "run.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace riley {

namespace {

constexpr int kDefaultKeyDelayMs = 20;
constexpr int kMaxKeyDelayMs = 2000;
constexpr int kMaxTextLength = 4096;
constexpr qint64 kSessionQuietMs = 10 * 60 * 1000;
constexpr int kTreeDepth = 30;
constexpr int kTreeNodes = 2000;

const QRegularExpression &addressPattern()
{
    static const QRegularExpression re(QStringLiteral("^0x[0-9a-f]{1,16}$"));
    return re;
}

bool readRequestLine(int fd, QByteArray *line)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    char buffer[4096];
    while (!line->contains('\n')) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline
                                                                               - std::chrono::steady_clock::now());
        if (ms.count() <= 0 || line->size() > (1 << 20))
            return false;
        pollfd pfd{fd, POLLIN, 0};
        const int r = poll(&pfd, 1, int(ms.count()));
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        const ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        line->append(buffer, n);
    }
    line->truncate(line->indexOf('\n'));
    return true;
}

void sendAll(int fd, const QByteArray &data)
{
    const char *p = data.constData();
    qsizetype left = data.size();
    while (left > 0) {
        const ssize_t n = ::send(fd, p, size_t(left), MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        p += n;
        left -= n;
    }
}

// The caller hung up (its command was ended): the action it asked for ends too.
bool peerGone(int fd)
{
    pollfd pfd{fd, POLLRDHUP, 0};
    return poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLRDHUP | POLLHUP | POLLERR));
}

QString oneLine(QString s)
{
    for (QChar &c : s)
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            c = QLatin1Char(' ');
    return s;
}

QString refusedLine(const Decision &d)
{
    return QStringLiteral("refused %1 (rule %2)").arg(d.reason, d.rule);
}

// Characters of TEXT as they go out, one key event each: code points (a surrogate pair is one).
QStringList keyUnits(const QString &text)
{
    QStringList units;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
            units << text.mid(i, 2);
            ++i;
        } else {
            units << QString(text.at(i));
        }
    }
    return units;
}

QStringList wtypeArgs(const QString &unit)
{
    if (unit == QLatin1String("\n"))
        return {QStringLiteral("wtype"), QStringLiteral("-k"), QStringLiteral("Return")};
    if (unit == QLatin1String("\t"))
        return {QStringLiteral("wtype"), QStringLiteral("-k"), QStringLiteral("Tab")};
    return {QStringLiteral("wtype"), QStringLiteral("--"), unit};
}

// "ctrl+shift+t" -> wtype -M ctrl -M shift -k t -m shift -m ctrl. Empty when the name is not acceptable.
QStringList keyArgs(const QString &name)
{
    static const QRegularExpression keyName(QStringLiteral("^[A-Za-z0-9_]{1,32}$"));
    QStringList parts = name.split(QLatin1Char('+'));
    if (parts.isEmpty() || parts.size() > 5)
        return {};
    const QString key = parts.takeLast();
    if (!keyName.match(key).hasMatch())
        return {};
    QStringList mods;
    for (const QString &m : parts) {
        const QString l = m.toLower();
        if (l == QLatin1String("ctrl") || l == QLatin1String("shift") || l == QLatin1String("alt"))
            mods << l;
        else if (l == QLatin1String("super") || l == QLatin1String("logo"))
            mods << QStringLiteral("logo");
        else
            return {};
    }
    QStringList args{QStringLiteral("wtype")};
    for (const QString &m : mods)
        args << QStringLiteral("-M") << m;
    args << QStringLiteral("-k") << key;
    for (auto it = mods.crbegin(); it != mods.crend(); ++it)
        args << QStringLiteral("-m") << *it;
    return args;
}

// A deny by a rule before R9 (password field): the password facts cannot change it.
bool decidedBeforePasswordRule(const Decision &d)
{
    if (d.verdict != Verdict::Deny)
        return false;
    static const QStringList early = {QStringLiteral("no-config"), QStringLiteral("invalid-config"),
                                      QStringLiteral("stopped"), QStringLiteral("unknown-client"),
                                      QStringLiteral("unknown-action")};
    return early.contains(d.reason) || d.reason.startsWith(QLatin1String("deny-"));
}

QString notifyText(const QString &client)
{
    return QStringLiteral("%1 started a session: it may now see and act as config.xml allows. "
                          "riley-agent stop ends it.")
        .arg(client);
}

} // namespace

// One allowed action in progress.
struct Service::Job {
    int fd = -1;
    Request request;
    Decision decision;
    Desktop desktop;
    const HyprWindow *window = nullptr;
    QJsonObject args;
    quint64 generation = 0;
    int keysSent = 0;
};

Service::Service(ServiceOptions options, QByteArray secret)
    : m_options(std::move(options)), m_secret(std::move(secret))
{
    m_bar.since = QDateTime::currentDateTimeUtc();
    // A stop in force when the service last ran is still in force. Anything but a clear "no such file" (a file
    // that cannot be checked, or a path that cannot be known) counts as stopped.
    struct stat st {};
    const QByteArray native = QFile::encodeName(m_options.stopPath);
    if (native.isEmpty() || ::lstat(native.constData(), &st) == 0 || errno != ENOENT) {
        m_stopped.store(true);
        m_bar = BarState{BarActivity::Stopped, QString(), QString(), QDateTime::currentDateTimeUtc()};
    }
}

void Service::setConfig(const ConfigLoad &load)
{
    std::lock_guard lock(m_configMutex);
    m_config = load;
}

ConfigLoad Service::config() const
{
    std::lock_guard lock(m_configMutex);
    return m_config;
}

// --- bar, sessions, stop -----------------------------------------------------------------------------------------

void Service::writeBarLocked()
{
    QString error;
    if (!writeBarState(m_options.statePath, m_bar, &error))
        std::fprintf(stderr, "riley-agentd: %s\n", qPrintable(error));
}

void Service::writeRestState()
{
    std::lock_guard lock(m_stateMutex);
    if (m_stopped.load()) {
        if (m_bar.activity != BarActivity::Stopped)
            m_bar = BarState{BarActivity::Stopped, QString(), QString(), QDateTime::currentDateTimeUtc()};
    } else {
        m_bar = BarState{BarActivity::Idle, QString(), QString(), QDateTime::currentDateTimeUtc()};
    }
    writeBarLocked();
}

// Written and flushed to disk before the stop is answered, so a service killed right after still starts stopped.
bool Service::writeStopFile(const QString &client, QString *error)
{
    const QString dir = QFileInfo(m_options.stopPath).absolutePath();
    if (!QDir().mkpath(dir)) {
        *error = QStringLiteral("cannot create ") + dir;
        return false;
    }
    ::chmod(QFile::encodeName(dir).constData(), 0700);
    const int fd = ::open(QFile::encodeName(m_options.stopPath).constData(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        *error = QStringLiteral("cannot write ") + m_options.stopPath;
        return false;
    }
    const QByteArray text = QStringLiteral("stopped %1 by %2\n")
                                .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate),
                                     client.isEmpty() ? QStringLiteral("unknown") : client)
                                .toUtf8();
    const bool ok = ::write(fd, text.constData(), size_t(text.size())) == ssize_t(text.size()) && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok)
        *error = QStringLiteral("cannot write ") + m_options.stopPath;
    return ok;
}

void Service::beginActing(const QString &client, const QString &action)
{
    bool newSession = false;
    {
        std::lock_guard lock(m_stateMutex);
        if (m_stopped.load())
            return;
        newSession = !m_sessions.contains(client);
        m_sessions.insert(client, QDateTime::currentMSecsSinceEpoch());
        m_bar = BarState{BarActivity::Acting, client, action, QDateTime::currentDateTimeUtc()};
        writeBarLocked();
    }
    if (newSession) {
        const QStringList argv{QStringLiteral("notify-send"), QStringLiteral("--app-name=riley-desktop-agent"),
                               QStringLiteral("Riley desktop agent"), notifyText(client)};
        std::thread([argv] { runProgram(argv, 5000); }).detach();
    }
}

void Service::endActing(const QString &client)
{
    std::lock_guard lock(m_stateMutex);
    if (m_stopped.load())
        return;
    m_sessions.insert(client, QDateTime::currentMSecsSinceEpoch());
    m_bar = BarState{BarActivity::Watching, client, QString(), QDateTime::currentDateTimeUtc()};
    writeBarLocked();
}

void Service::tick()
{
    std::lock_guard lock(m_stateMutex);
    if (m_stopped.load() || m_bar.activity == BarActivity::Acting)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_sessions.begin(); it != m_sessions.end();)
        it = now - it.value() > kSessionQuietMs ? m_sessions.erase(it) : std::next(it);
    if (m_sessions.isEmpty() && m_bar.activity != BarActivity::Idle) {
        m_bar = BarState{BarActivity::Idle, QString(), QString(), QDateTime::currentDateTimeUtc()};
        writeBarLocked();
    }
}

void Service::stop(const QString &client, const QString &word)
{
    {
        std::lock_guard lock(m_stateMutex);
        {
            std::lock_guard stopLock(m_stopMutex);
            m_stopped.store(true);
            m_stopGeneration.fetch_add(1);
        }
        m_sessions.clear();
        m_bar = BarState{BarActivity::Stopped, client, QString(), QDateTime::currentDateTimeUtc()};
        writeBarLocked();
    }
    m_stopCondition.notify_all();
    QString error;
    if (!writeStopFile(client, &error))   // the stop holds in this run either way
        std::fprintf(stderr, "riley-agentd: %s: the stop will not survive a restart\n", qPrintable(error));
    logControl(client, word, QStringLiteral("done"));
}

void Service::resume(const QString &client)
{
    {
        std::lock_guard lock(m_stateMutex);
        if (::unlink(QFile::encodeName(m_options.stopPath).constData()) != 0 && errno != ENOENT)
            std::fprintf(stderr, "riley-agentd: cannot remove %s: a restart will start stopped\n",
                         qPrintable(m_options.stopPath));
        m_stopped.store(false);
        m_sessions.clear();
        m_bar = BarState{BarActivity::Idle, QString(), QString(), QDateTime::currentDateTimeUtc()};
        writeBarLocked();
    }
    logControl(client, QStringLiteral("resume"), QStringLiteral("done"));
}

bool Service::waitMs(int ms, quint64 generation)
{
    std::unique_lock lock(m_stopMutex);
    m_stopCondition.wait_for(lock, std::chrono::milliseconds(ms), [&] { return stoppedSince(generation); });
    return !stoppedSince(generation);
}

// --- log --------------------------------------------------------------------------------------------------------

void Service::log(const Request &request, const Decision &decision, const QString &result)
{
    std::lock_guard lock(m_logMutex);
    QString error;
    if (!appendLog(m_options.logPath, makeLogRecord(request, decision, result, m_secret), &error))
        std::fprintf(stderr, "riley-agentd: %s\n", qPrintable(error));
}

void Service::logControl(const QString &client, const QString &action, const QString &result)
{
    std::lock_guard lock(m_logMutex);
    QString error;
    const QString line = controlLogLine(QDateTime::currentDateTimeUtc(), client.isEmpty() ? QStringLiteral("unknown")
                                                                                          : client,
                                        action, result);
    if (!appendLogLine(m_options.logPath, line, &error))
        std::fprintf(stderr, "riley-agentd: %s\n", qPrintable(error));
}

// --- requests ---------------------------------------------------------------------------------------------------

WindowInfo Service::windowInfo(const HyprWindow &w, const Desktop &desktop) const
{
    WindowInfo info;
    info.windowClass = w.windowClass;
    info.title = w.title;
    info.monitor = w.monitor;
    info.agent = windowAgent(w.pid);
    info.focused = !desktop.activeAddress.isEmpty() && desktop.activeAddress == w.address;
    return info;
}

void Service::serve(int fd)
{
    ucred cred{};
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != getuid()) {
        ::close(fd);
        return;
    }
    QByteArray line;
    QJsonObject request;
    if (readRequestLine(fd, &line))
        request = QJsonDocument::fromJson(line).object();
    const QString action = request.value(QStringLiteral("action")).toString();
    const QString client = callerKind(cred.pid, m_options.ownLoopExe);

    Reply reply;
    if (action == QLatin1String("stop") || action == QLatin1String("segfault")) {
        stop(client, action);
        reply = {QStringLiteral("ok"), QStringLiteral("ok stopped: every permission is revoked until riley-agent resume"),
                 {}, {}};
    } else if (action == QLatin1String("resume")) {
        // R0: only the user may end a stop; a caller under a coding agent or the tool's own loop is refused.
        Request ask;
        // A caller with no client of its own but named like the loop is refused as the loop is (rule R0). The log
        // still names the client it really is.
        ask.client = client.isEmpty() && loopNamedAncestor(cred.pid) ? QStringLiteral("own-loop") : client;
        ask.action = action;
        State state;
        state.stopped = m_stopped.load();
        const Decision d = decide(ask, config(), state);
        if (d.verdict == Verdict::Allow) {
            resume(client);
            reply = {QStringLiteral("ok"), QStringLiteral("ok resumed: permissions as config.xml says"), {}, {}};
        } else {
            logControl(client, action, QStringLiteral("refused"));
            reply = {QStringLiteral("refused"),
                     refusedLine(d) + QStringLiteral(": only you can resume, from your own terminal or the tool's "
                                                     "window"),
                     {}, {}};
        }
    } else if (action.isEmpty()) {
        reply = {QStringLiteral("failed"), QStringLiteral("failed bad-request"), {}, {}};
    } else {
        reply = handleAction(fd, client, action, request);
    }

    QJsonObject out{{QStringLiteral("kind"), reply.kind}, {QStringLiteral("line"), reply.line}};
    if (!reply.body.isEmpty())
        out.insert(QStringLiteral("body"), QJsonArray::fromStringList(reply.body));
    if (!reply.png.isEmpty())
        out.insert(QStringLiteral("png"), QString::fromLatin1(reply.png.toBase64()));
    sendAll(fd, QJsonDocument(out).toJson(QJsonDocument::Compact) + '\n');
    ::close(fd);
}

Service::Reply Service::handleAction(int fd, const QString &client, const QString &action, const QJsonObject &args)
{
    // One action at a time. Stop and resume never wait for this lock; a request that waits here is decided after it
    // gets it, with the facts and the stopped state of that moment.
    std::unique_lock actionLock(m_actionMutex, std::defer_lock);
    if (!actionLock.try_lock_for(std::chrono::seconds(30)))
        return {QStringLiteral("failed"), QStringLiteral("failed busy: another action is still running"), {}, {}};

    Job job;
    job.fd = fd;
    job.args = args;
    job.request.client = client;
    job.request.action = action;
    const QString address = args.value(QStringLiteral("window")).toString();
    const bool needsWindow = knownActions().contains(action) && action != QLatin1String("windows")
                             && action != QLatin1String("launch");

    job.desktop = readDesktop();
    if (needsWindow) {
        job.window = addressPattern().match(address).hasMatch() ? job.desktop.window(address) : nullptr;
        if (!job.window) {
            const Decision none{Verdict::Deny, QStringLiteral("no-window"), QStringLiteral("service")};
            log(job.request, none, QStringLiteral("failed"));
            return {QStringLiteral("failed"),
                    job.desktop.ok ? QStringLiteral("failed no-window: no window has address %1").arg(oneLine(address))
                                   : QStringLiteral("failed no-desktop: %1").arg(job.desktop.error),
                    {}, {}};
        }
        job.request.window = windowInfo(*job.window, job.desktop);
    }
    if (action == QLatin1String("launch"))
        job.request.window.windowClass = args.value(QStringLiteral("app")).toString();
    if (action == QLatin1String("type"))
        job.request.text = args.value(QStringLiteral("text")).toString();
    if (action == QLatin1String("key"))
        job.request.text = args.value(QStringLiteral("key")).toString();
    job.generation = m_stopGeneration.load();
    const ConfigLoad load = config();
    State state;
    state.stopped = m_stopped.load();

    if ((action == QLatin1String("type") || action == QLatin1String("key")) && job.request.window.focused) {
        // The password facts are read only for R9 and later: a request an earlier rule refuses (no config, stopped,
        // unknown client, deny-always) is decided without asking the accessibility bus.
        if (!decidedBeforePasswordRule(decide(job.request, load, state))) {
            // Unknown (no accessibility bus, the app is not on it, or nothing in it has focus) refuses typing (R9).
            const a11y::Field field = a11y::focusedField(long(job.window->pid));
            job.request.passwordField = field == a11y::Field::Password;
            job.request.passwordUnknown = field == a11y::Field::Unknown;
        }
    }

    if (action == QLatin1String("windows")) {
        // The listing itself is decided per window below; here only what holds for every window (R1 to R5).
        const Decision general = decide(job.request, load, state);
        if (general.verdict == Verdict::Deny
            && (general.rule == QLatin1String("R1") || general.rule == QLatin1String("R2")
                || general.rule == QLatin1String("R3") || general.rule == QLatin1String("R4"))) {
            log(job.request, general, QStringLiteral("refused"));
            return {QStringLiteral("refused"), refusedLine(general), {}, {}};
        }
        return doWindows(client, job.desktop, job.generation);
    }

    job.decision = decide(job.request, load, state);
    if (job.decision.verdict == Verdict::Deny) {
        log(job.request, job.decision, QStringLiteral("refused"));
        return {QStringLiteral("refused"), refusedLine(job.decision), {}, {}};
    }

    beginActing(client, action);
    Reply reply = runAllowed(job);
    endActing(client);
    return reply;
}

Service::Reply Service::runAllowed(Job &job)
{
    const QString &action = job.request.action;
    Reply reply;
    if (action == QLatin1String("type"))
        reply = doType(job);
    else if (action == QLatin1String("key"))
        reply = doKey(job);
    else if (action == QLatin1String("click"))
        reply = doClick(job);
    else if (action == QLatin1String("point"))
        reply = doPoint(job);
    else if (action == QLatin1String("see"))
        reply = doSee(job);
    else if (action == QLatin1String("tree"))
        reply = doTree(job);
    else if (action == QLatin1String("launch"))
        reply = doLaunch(job);
    else if (action == QLatin1String("close"))
        reply = doClose(job);
    else
        reply = {QStringLiteral("failed"), QStringLiteral("failed not-implemented"), {}, {}};

    // A stop that came while acting wins over whatever the action reported.
    if (stoppedSince(job.generation) && reply.kind == QLatin1String("ok"))
        reply = {QStringLiteral("stopped"), QStringLiteral("stopped by stop or segfault"), {}, {}};
    if (reply.kind == QLatin1String("stopped"))
        reply.png.clear();
    if (reply.kind != QLatin1String("refused"))   // a refusal midway has logged its own decision
        log(job.request, job.decision,
            reply.kind == QLatin1String("ok") ? QStringLiteral("done") : reply.kind);
    return reply;
}

Service::Reply Service::doWindows(const QString &client, const Desktop &desktop, quint64 generation)
{
    const ConfigLoad load = config();
    State state;
    state.stopped = m_stopped.load();
    Request request;
    request.client = client;
    request.action = QStringLiteral("windows");
    QStringList body;
    for (const HyprWindow &w : desktop.windows) {
        Request one = request;
        one.window = windowInfo(w, desktop);
        if (decide(one, load, state).verdict == Verdict::Allow)
            body << QStringLiteral("%1 %2 %3 %4,%5 %6x%7 \"%8\"")
                        .arg(w.address, oneLine(w.windowClass), w.monitor)
                        .arg(w.x).arg(w.y).arg(w.width).arg(w.height)
                        .arg(oneLine(w.title));
    }
    // Each window was decided on its own; the listing holds only those allowed.
    const Decision decision{Verdict::Allow, QStringLiteral("allowed"), QStringLiteral("per-window")};
    if (stoppedSince(generation)) {
        log(request, decision, QStringLiteral("stopped"));
        return {QStringLiteral("stopped"), QStringLiteral("stopped by stop or segfault"), {}, {}};
    }
    log(request, decision, QStringLiteral("done"));
    return {QStringLiteral("ok"), QStringLiteral("ok %1 windows").arg(body.size()), body, {}};
}

// The re-check right before every key event: the active window must still be the target, and the engine is asked
// again with what is true now (focus, config, stopped). False with *reply set when the typing must end.
bool Service::recheckBeforeKey(Job &job, Reply *reply, int sent)
{
    if (stoppedSince(job.generation)) {
        *reply = {QStringLiteral("stopped"), QStringLiteral("stopped after %1 key events").arg(sent), {}, {}};
        return false;
    }
    if (peerGone(job.fd)) {
        *reply = {QStringLiteral("failed"), QStringLiteral("failed cancelled after %1 key events").arg(sent), {}, {}};
        return false;
    }
    Request now = job.request;
#ifndef RILEY_FAULT_FOCUS_RECHECK_REMOVED
    bool ok = false;
    const QString active = activeWindowAddress(&ok);
    now.focusMoved = !ok || active != job.window->address;
#endif
    State state;
    state.stopped = m_stopped.load();
    const Decision again = decide(now, config(), state);
    if (again.verdict == Verdict::Allow)
        return true;
    if (again.reason == QLatin1String("stopped")) {
        *reply = {QStringLiteral("stopped"), QStringLiteral("stopped after %1 key events").arg(sent), {}, {}};
        return false;
    }
    log(now, again, QStringLiteral("refused"));
    *reply = {QStringLiteral("refused"), refusedLine(again) + QStringLiteral(" after %1 key events").arg(sent), {}, {}};
    return false;
}

Service::Reply Service::doType(Job &job)
{
    const QString &text = job.request.text;
    if (text.isEmpty() || text.size() > kMaxTextLength)
        return {QStringLiteral("failed"), QStringLiteral("failed bad-text: 1 to %1 characters").arg(kMaxTextLength), {},
                {}};
    for (const QChar c : text)
        if ((c.unicode() < 0x20 && c != QLatin1Char('\n') && c != QLatin1Char('\t')) || c.unicode() == 0x7f)
            return {QStringLiteral("failed"), QStringLiteral("failed bad-text: control characters"), {}, {}};
    const int delay = std::clamp(job.args.value(QStringLiteral("delay-ms")).toInt(kDefaultKeyDelayMs), 0,
                                 kMaxKeyDelayMs);
    const QStringList units = keyUnits(text);
    int sent = 0;
    for (const QString &unit : units) {
        Reply reply;
        if (!recheckBeforeKey(job, &reply, sent))
            return reply;
        const RunResult r = runProgram(wtypeArgs(unit), 3000);
        if (r.exitCode != 0)
            return {QStringLiteral("failed"), QStringLiteral("failed wtype after %1 key events").arg(sent), {}, {}};
        ++sent;
        if (sent < units.size())
            waitMs(delay, job.generation);
    }
    return {QStringLiteral("ok"), QStringLiteral("ok typed %1 key events").arg(sent), {}, {}};
}

Service::Reply Service::doKey(Job &job)
{
    const QStringList args = keyArgs(job.request.text);
    if (args.isEmpty())
        return {QStringLiteral("failed"), QStringLiteral("failed bad-key: a key name such as Return or ctrl+a"), {}, {}};
    Reply reply;
    if (!recheckBeforeKey(job, &reply, 0))
        return reply;
    const RunResult r = runProgram(args, 3000);
    if (r.exitCode != 0)
        return {QStringLiteral("failed"), QStringLiteral("failed wtype"), {}, {}};
    return {QStringLiteral("ok"), QStringLiteral("ok key %1").arg(job.request.text), {}, {}};
}

bool Service::moveCursor(Job &job, int gx, int gy, QString *error)
{
    if (!hyprDispatch(QStringLiteral("hl.dsp.cursor.move({x=%1, y=%2})").arg(gx).arg(gy), error))
        return false;
    if (!waitMs(60, job.generation))
        return false;
    const QJsonDocument pos = QJsonDocument::fromJson(hyprRequest("j/cursorpos", error));
    const int x = pos.object().value(QStringLiteral("x")).toInt(-1000000);
    const int y = pos.object().value(QStringLiteral("y")).toInt(-1000000);
    if (std::abs(x - gx) > 1 || std::abs(y - gy) > 1) {
        if (error)
            *error = QStringLiteral("the pointer is at %1,%2, not %3,%4").arg(x).arg(y).arg(gx).arg(gy);
        return false;
    }
    return true;
}

// A point inside the window that no other window covers (window-local logical pixels); empty when fine.
static QString pointProblem(const Desktop &d, const HyprWindow &w, int x, int y)
{
    if (x < 0 || y < 0 || x >= w.width || y >= w.height)
        return QStringLiteral("outside-window: %1,%2 is not inside %3x%4").arg(x).arg(y).arg(w.width).arg(w.height);
    if (w.hidden || !w.mapped)
        return QStringLiteral("hidden: the window is not shown");
    const int gx = w.x + x, gy = w.y + y;
    for (const HyprWindow &o : d.windows) {
        if (o.address == w.address || o.hidden || !o.mapped)
            continue;
        const bool sameSpace = o.workspaceId == w.workspaceId || (o.pinned && o.monitorId == w.monitorId);
        if (!sameSpace)
            continue;
        const bool covers = gx >= o.x && gx < o.x + o.width && gy >= o.y && gy < o.y + o.height;
        if (covers && (o.fullscreen || ((o.floating || o.pinned) && !w.fullscreen)))
            return QStringLiteral("covered: %1 covers that point").arg(oneLine(o.windowClass));
    }
    return QString();
}

Service::Reply Service::doPoint(Job &job)
{
    const int x = job.args.value(QStringLiteral("x")).toInt(-1), y = job.args.value(QStringLiteral("y")).toInt(-1);
    const QString problem = pointProblem(job.desktop, *job.window, x, y);
    if (!problem.isEmpty())
        return {QStringLiteral("failed"), QStringLiteral("failed ") + problem, {}, {}};
    QString error;
    if (!moveCursor(job, job.window->x + x, job.window->y + y, &error))
        return stoppedSince(job.generation)
                   ? Reply{QStringLiteral("stopped"), QStringLiteral("stopped before the pointer moved"), {}, {}}
                   : Reply{QStringLiteral("failed"), QStringLiteral("failed pointer: ") + error, {}, {}};
    return {QStringLiteral("ok"), QStringLiteral("ok pointer at %1,%2 in the window").arg(x).arg(y), {}, {}};
}

Service::Reply Service::doClick(Job &job)
{
    const int x = job.args.value(QStringLiteral("x")).toInt(-1), y = job.args.value(QStringLiteral("y")).toInt(-1);
    const QString buttonName = job.args.value(QStringLiteral("button")).toString(QStringLiteral("left"));
    const int button = buttonName == QLatin1String("right") ? 2 : buttonName == QLatin1String("middle") ? 3 : 1;
    const QString problem = pointProblem(job.desktop, *job.window, x, y);
    if (!problem.isEmpty())
        return {QStringLiteral("failed"), QStringLiteral("failed ") + problem, {}, {}};
    if (!m_mouse.isOpen())
        return {QStringLiteral("failed"), QStringLiteral("failed no-mouse: /dev/uinput could not be opened"), {}, {}};
    if (const int left = m_mouse.warmupLeftMs(); left > 0 && !waitMs(left, job.generation))
        return {QStringLiteral("stopped"), QStringLiteral("stopped before the click"), {}, {}};
    QString error;
    if (!moveCursor(job, job.window->x + x, job.window->y + y, &error)) {
        if (stoppedSince(job.generation))
            return {QStringLiteral("stopped"), QStringLiteral("stopped before the click"), {}, {}};
        return {QStringLiteral("failed"), QStringLiteral("failed pointer: ") + error, {}, {}};
    }
    if (stoppedSince(job.generation))
        return {QStringLiteral("stopped"), QStringLiteral("stopped before the click"), {}, {}};
    if (!m_mouse.click(button, &error))
        return {QStringLiteral("failed"), QStringLiteral("failed click: ") + error, {}, {}};
    return {QStringLiteral("ok"), QStringLiteral("ok clicked %1 at %2,%3 in the window").arg(buttonName).arg(x).arg(y),
            {}, {}};
}

Service::Reply Service::doSee(Job &job)
{
    const HyprWindow &w = *job.window;
    if (w.hidden || !w.mapped || w.width <= 0 || w.height <= 0)
        return {QStringLiteral("failed"), QStringLiteral("failed hidden: the window is not shown"), {}, {}};
    const QString geometry = QStringLiteral("%1,%2 %3x%4").arg(w.x).arg(w.y).arg(w.width).arg(w.height);
    const RunResult r = runProgram({QStringLiteral("grim"), QStringLiteral("-g"), geometry, QStringLiteral("-")}, 5000);
    if (r.exitCode != 0 || !r.out.startsWith("\x89PNG"))
        return {QStringLiteral("failed"), QStringLiteral("failed grim: no picture"), {}, {}};
    if (stoppedSince(job.generation))
        return {QStringLiteral("stopped"), QStringLiteral("stopped before the picture was sent"), {}, {}};
    Reply reply{QStringLiteral("ok"), QStringLiteral("ok picture of %1x%2 logical pixels").arg(w.width).arg(w.height),
                {}, r.out};
    return reply;
}

Service::Reply Service::doTree(Job &job)
{
    std::vector<a11y::Node> nodes;
    std::string error;
    const bool done = a11y::tree(long(job.window->pid), job.window->title.toStdString(), kTreeDepth, kTreeNodes,
                                 &m_stopped, &nodes, &error);
    if (stoppedSince(job.generation))
        return {QStringLiteral("stopped"), QStringLiteral("stopped while reading the tree"), {}, {}};
    if (!done)
        return {QStringLiteral("failed"), QStringLiteral("failed tree: ") + QString::fromStdString(error), {}, {}};
    QStringList body;
    for (const a11y::Node &n : nodes)
        body << QString(n.depth * 2, QLatin1Char(' ')) + oneLine(QString::fromStdString(n.role)) + QStringLiteral(": ")
                    + oneLine(QString::fromStdString(n.name));
    return {QStringLiteral("ok"), QStringLiteral("ok %1 nodes").arg(body.size()), body, {}};
}

Service::Reply Service::doLaunch(Job &job)
{
    static const QRegularExpression appName(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$"));
    const QString app = job.request.window.windowClass;
    if (!appName.match(app).hasMatch())
        return {QStringLiteral("failed"), QStringLiteral("failed bad-app: a program name, no path or arguments"), {}, {}};
    // A clean, minimal environment and the home folder: never the caller's folder or environment.
    const QString path = QStringLiteral("/usr/local/bin:/usr/bin:/bin");
    const QString exe = QStandardPaths::findExecutable(app, path.split(QLatin1Char(':')));
    if (exe.isEmpty())
        return {QStringLiteral("failed"), QStringLiteral("failed no-such-app: %1 is not in %2").arg(app, path), {}, {}};
    const QString home = QDir::homePath();
    QStringList env{QStringLiteral("HOME=") + home, QStringLiteral("PATH=") + path};
    for (const char *name : {"USER", "LOGNAME", "LANG", "XDG_RUNTIME_DIR", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS",
                             "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "DISPLAY"}) {
        const QString value = qEnvironmentVariable(name);
        if (!value.isEmpty())
            env << QString::fromLatin1(name) + QLatin1Char('=') + value;
    }
    const qint64 pid = spawnDetached({exe}, home, env);
    if (pid < 0)
        return {QStringLiteral("failed"), QStringLiteral("failed launch: could not start %1").arg(app), {}, {}};
    return {QStringLiteral("ok"), QStringLiteral("ok launched %1 (pid %2) in %3").arg(app).arg(pid).arg(home), {}, {}};
}

Service::Reply Service::doClose(Job &job)
{
    QString error;
    if (!hyprDispatch(QStringLiteral("hl.dsp.window.close({ window = \"address:%1\" })").arg(job.window->address),
                      &error))
        return {QStringLiteral("failed"), QStringLiteral("failed close: ") + error, {}, {}};
    return {QStringLiteral("ok"), QStringLiteral("ok asked the window to close"), {}, {}};
}

} // namespace riley
