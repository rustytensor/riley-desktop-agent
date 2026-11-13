// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-agent: the command line coding agents use. Each call sends one request to riley-agentd and prints one
// result. The first line says what happened: "ok ...", "refused <reason> ...", "stopped ..." or "failed ...".
// Exit status: 0 ok, 1 refused, 3 stopped, 4 failed, 2 usage or no service.
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

const char *kHelp =
    "riley-agent: see and act on the Hyprland desktop through riley-agentd, as config.xml allows.\n"
    "One call, one result. Plain text out. Every call is checked and logged by the service.\n"
    "\n"
    "Commands:\n"
    "  windows                      list the windows you may see, one per line:\n"
    "                               ADDR class monitor x,y wxh \"title\"\n"
    "  see    -w ADDR -p FILE       a PNG of the window, written to FILE; FILE must be in a 0700 folder\n"
    "                               under $XDG_RUNTIME_DIR (never in a project folder; delete it after use)\n"
    "  tree   -w ADDR               the window's controls from the accessibility tree, \"role: name\",\n"
    "                               indented two spaces per level\n"
    "  point  -w ADDR X Y           move the pointer to X,Y in the window\n"
    "  click  -w ADDR X Y [-b left|right|middle]   click at X,Y in the window (left by default)\n"
    "  type   -w ADDR [-d MS] TEXT  type TEXT into the window's focused control (quote it; MS between keys)\n"
    "  key    -w ADDR NAME          press one key: Return, Tab, Escape, BackSpace, ctrl+a, ctrl+shift+t\n"
    "  launch APP                   start a program from /usr/bin, in your home folder, clean environment\n"
    "  close  -w ADDR               ask the window to close\n"
    "  stop                         revoke every permission and end what is running, at once\n"
    "  segfault                     the same as stop (the safe word)\n"
    "  resume                       end a stop; refused from a coding agent, only the user may resume\n"
    "  help                         this text\n"
    "\n"
    "Windows: run `riley-agent windows` first and use the window's address (ADDR, 0x...) with -w.\n"
    "Coordinates are logical pixels inside the window: 0,0 is its top-left corner, wxh its size.\n"
    "Typing goes only to the window that has focus; click into the field first. type and key are\n"
    "refused for password fields, for fields the service cannot check, and for text that looks like\n"
    "a key, token or card number. Never type passwords or keys.\n"
    "\n"
    "Result: the first line is one of\n"
    "  ok ...                      done (exit 0)\n"
    "  refused REASON (rule R)     not allowed; REASON says why, R is the rule that decided (exit 1)\n"
    "  stopped ...                 a stop is in force or came during the action (exit 3)\n"
    "  failed WHAT: ...            allowed, but it did not work (exit 4)\n"
    "Exit 2: bad usage, or riley-agentd is not running.\n"
    "\n"
    "Common refusals: unknown-client (the config does not list you), no-permission (nothing allows this\n"
    "action on this window), remote-not-allowed (you count as remote: you may only see and act on\n"
    "windows on the remote allow-list), deny-* (never allowed: lock screen, password managers,\n"
    "banking and payment, system settings, agent terminals, this tool), stopped, no-config.\n"
    "A refusal is final for that request: do not retry it or work around it; tell the user.\n"
    "\n"
    "Options: -w/--window ADDR, -p/--png FILE, -b/--button NAME, -d/--delay-ms MS, --socket PATH.\n";

int usage(const char *problem)
{
    std::fprintf(stderr, "riley-agent: %s (riley-agent help shows every command)\n", problem ? problem : "usage");
    return 2;
}

QString defaultSocketPath()
{
    const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        return QString();
    return QString::fromLocal8Bit(runtime) + QStringLiteral("/riley-desktop-agent/agentd.sock");
}

bool toInt(const QString &s, int *out)
{
    bool ok = false;
    *out = s.toInt(&ok);
    return ok;
}

// Pictures go only into a private folder under $XDG_RUNTIME_DIR, never into a project folder.
bool picturePathAllowed(const QString &path, QString *why)
{
    const QString runtime = QFileInfo(QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR"))).canonicalFilePath();
    const QString dir = QFileInfo(QFileInfo(path).absolutePath()).canonicalFilePath();
    if (runtime.isEmpty() || dir.isEmpty() || !(dir + QLatin1Char('/')).startsWith(runtime + QLatin1Char('/'))) {
        *why = QStringLiteral("the picture must go in a folder under $XDG_RUNTIME_DIR");
        return false;
    }
    struct stat st {};
    if (::stat(QFile::encodeName(dir).constData(), &st) != 0 || st.st_uid != ::getuid() || (st.st_mode & 077) != 0) {
        *why = QStringLiteral("the picture's folder must be yours with mode 0700");
        return false;
    }
    return true;
}

bool writePicture(const QString &path, const QByteArray &png)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                          0600);
    if (fd < 0)
        return false;
    const char *p = png.constData();
    qsizetype left = png.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, size_t(left));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        p += n;
        left -= n;
    }
    return ::close(fd) == 0;
}

} // namespace

int main(int argc, char **argv)
{
    QStringList args;
    for (int i = 1; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);

    QString socketPath = defaultSocketPath();
    if (args.size() >= 2 && args.first() == QLatin1String("--socket")) {
        socketPath = args.at(1);
        args.remove(0, 2);
    }
    if (args.isEmpty())
        return usage("no command");
    const QString command = args.takeFirst();
    if (command == QLatin1String("help") || command == QLatin1String("--help") || command == QLatin1String("-h")) {
        std::fputs(kHelp, stdout);
        return 0;
    }

    QJsonObject request{{QStringLiteral("action"), command}};
    QString png;
    QStringList positional;
    while (!args.isEmpty()) {
        const QString a = args.takeFirst();
        const auto is = [&a](const char *shortName, const char *longName) {
            return a == QLatin1String(shortName) || a == QLatin1String(longName);
        };
        if (is("-w", "--window") && !args.isEmpty())
            request.insert(QStringLiteral("window"), args.takeFirst());
        else if (is("-p", "--png") && !args.isEmpty())
            png = args.takeFirst();
        else if (is("-b", "--button") && !args.isEmpty())
            request.insert(QStringLiteral("button"), args.takeFirst());
        else if (is("-d", "--delay-ms") && !args.isEmpty()) {
            int ms = 0;
            if (!toInt(args.takeFirst(), &ms) || ms < 0)
                return usage("--delay-ms needs a number of milliseconds");
            request.insert(QStringLiteral("delay-ms"), ms);
        } else if (a == QLatin1String("--")) {
            positional += args;
            args.clear();
        } else
            positional << a;
    }

    const bool windowed = command != QLatin1String("windows") && command != QLatin1String("launch")
                          && command != QLatin1String("stop") && command != QLatin1String("segfault")
                          && command != QLatin1String("resume");
    if (windowed && !request.contains(QStringLiteral("window")))
        return usage("this command needs -w ADDR (an address from riley-agent windows)");
    if (command == QLatin1String("click") || command == QLatin1String("point")) {
        int x = 0, y = 0;
        if (positional.size() != 2 || !toInt(positional.at(0), &x) || !toInt(positional.at(1), &y))
            return usage("click and point need X Y");
        request.insert(QStringLiteral("x"), x);
        request.insert(QStringLiteral("y"), y);
    } else if (command == QLatin1String("type")) {
        if (positional.size() != 1)
            return usage("type needs one TEXT (quote it)");
        request.insert(QStringLiteral("text"), positional.first());
    } else if (command == QLatin1String("key")) {
        if (positional.size() != 1)
            return usage("key needs one NAME");
        request.insert(QStringLiteral("key"), positional.first());
    } else if (command == QLatin1String("launch")) {
        if (positional.size() != 1)
            return usage("launch needs one APP");
        request.insert(QStringLiteral("app"), positional.first());
    } else if (command == QLatin1String("see")) {
        QString why;
        if (png.isEmpty())
            return usage("see needs -p FILE");
        if (!picturePathAllowed(png, &why))
            return usage(qPrintable(why));
    } else if (!positional.isEmpty()) {
        return usage("unexpected arguments");
    }

    const QByteArray native = QFile::encodeName(socketPath);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (native.isEmpty() || size_t(native.size()) >= sizeof addr.sun_path)
        return usage("no socket path (XDG_RUNTIME_DIR not set?)");
    std::memcpy(addr.sun_path, native.constData(), size_t(native.size()));
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        std::printf("failed no-service: riley-agentd is not running (%s)\n", native.constData());
        return 2;
    }
    const QByteArray out = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (::send(fd, out.constData(), size_t(out.size()), MSG_NOSIGNAL) != ssize_t(out.size())) {
        std::printf("failed no-service: could not send the request\n");
        return 2;
    }
    QByteArray reply;
    char buffer[65536];
    for (;;) {
        const ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        reply.append(buffer, n);
    }
    ::close(fd);
    const QJsonObject answer = QJsonDocument::fromJson(reply).object();
    const QString kind = answer.value(QStringLiteral("kind")).toString();
    QString line = answer.value(QStringLiteral("line")).toString();
    if (kind.isEmpty()) {
        std::printf("failed no-answer: riley-agentd closed the connection\n");
        return 4;
    }
    if (kind == QLatin1String("ok") && command == QLatin1String("see")) {
        const QByteArray picture = QByteArray::fromBase64(answer.value(QStringLiteral("png")).toString().toLatin1());
        if (picture.isEmpty() || !writePicture(png, picture)) {
            std::printf("failed picture: could not write %s\n", qPrintable(png));
            return 4;
        }
        line += QStringLiteral(", written to ") + png;
    }
    std::printf("%s\n", qPrintable(line));
    for (const QJsonValue &v : answer.value(QStringLiteral("body")).toArray())
        std::printf("%s\n", qPrintable(v.toString()));
    std::fflush(stdout);
    if (kind == QLatin1String("ok"))
        return 0;
    if (kind == QLatin1String("refused"))
        return 1;
    if (kind == QLatin1String("stopped"))
        return 3;
    return 4;
}
