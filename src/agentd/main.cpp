// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-agentd: the service. It owns every action and is the only path to the desktop.
//
//   riley-agentd [--config FILE] [--socket PATH] [--state-file PATH] [--log PATH] [--secret-file PATH]
//
// Every option only says where a file is (defaults in docs/files.md); none of them changes a check. Runs in the
// foreground until SIGTERM or SIGINT.
#include "barstate.h"
#include "config.h"
#include "log.h"
#include "service.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace riley;

namespace {

int usage(const char *problem)
{
    if (problem)
        std::fprintf(stderr, "riley-agentd: %s\n", problem);
    std::fprintf(stderr, "usage: riley-agentd [--config FILE] [--socket PATH] [--state-file PATH] [--log PATH] "
                         "[--secret-file PATH]\n");
    return 2;
}

QString defaultSocketPath()
{
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty())
        return QString();
    return runtime + QStringLiteral("/riley-desktop-agent/agentd.sock");
}

// The socket's folder must be this user's and closed to everyone else (0700). Made so when missing.
bool privateFolder(const QString &dir, QString *error)
{
    const QByteArray native = QFile::encodeName(dir);
    if (::mkdir(native.constData(), 0700) != 0 && errno != EEXIST) {
        *error = QStringLiteral("cannot create ") + dir;
        return false;
    }
    struct stat st {};
    if (::lstat(native.constData(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid()
        || (st.st_mode & 077) != 0) {
        *error = dir + QStringLiteral(" must be a folder of this user with mode 0700");
        return false;
    }
    return true;
}

int listenOn(const QString &path, QString *error)
{
    const QByteArray native = QFile::encodeName(path);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (size_t(native.size()) >= sizeof addr.sun_path) {
        *error = QStringLiteral("socket path too long");
        return -1;
    }
    std::memcpy(addr.sun_path, native.constData(), size_t(native.size()));
    struct stat st {};
    if (::lstat(native.constData(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            *error = path + QStringLiteral(" exists and is not a socket");
            return -1;
        }
        const int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        const bool live = probe >= 0 && ::connect(probe, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0;
        if (probe >= 0)
            ::close(probe);
        if (live) {
            *error = QStringLiteral("another riley-agentd is listening on ") + path;
            return -1;
        }
        ::unlink(native.constData());
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = QStringLiteral("no socket");
        return -1;
    }
    const mode_t old = ::umask(0177);
    const bool bound = ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0;
    ::umask(old);
    if (!bound || ::chmod(native.constData(), 0600) != 0 || ::listen(fd, 16) != 0) {
        *error = QStringLiteral("cannot listen on ") + path + QStringLiteral(": ") + QString::fromLocal8Bit(std::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

QString ownLoopExecutable()
{
    const QString dir = QCoreApplication::applicationDirPath();
    for (const QString &candidate : {dir + QStringLiteral("/riley-agent-loop"), dir + QStringLiteral("/../loop/riley-agent-loop")}) {
        const QFileInfo info(candidate);
        if (info.exists())
            return info.canonicalFilePath();
    }
    return QString();
}

} // namespace

int main(int argc, char **argv)
{
    // Qt's own event loop, not GLib's: the accessibility code drives GLib's default context itself, under its lock.
    qputenv("QT_NO_GLIB", "1");

    // SIGTERM and SIGINT arrive through a signalfd on the main thread; every thread inherits the blocked mask.
    sigset_t quit;
    sigemptyset(&quit);
    sigaddset(&quit, SIGTERM);
    sigaddset(&quit, SIGINT);
    pthread_sigmask(SIG_BLOCK, &quit, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    QCoreApplication app(argc, argv);
    ServiceOptions options;
    options.configPath = defaultConfigPath();
    options.socketPath = defaultSocketPath();
    options.statePath = defaultBarStatePath();
    options.logPath = defaultLogPath();
    options.secretPath = defaultSecretPath();
    const QStringList args = app.arguments();
    for (qsizetype i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (a == QLatin1String("-h") || a == QLatin1String("--help"))
            return usage(nullptr);
        if (i + 1 >= args.size())
            return usage("an option is missing its value");
        const QString value = QFileInfo(args.at(++i)).absoluteFilePath();
        if (a == QLatin1String("--config"))
            options.configPath = value;
        else if (a == QLatin1String("--socket"))
            options.socketPath = value;
        else if (a == QLatin1String("--state-file"))
            options.statePath = value;
        else if (a == QLatin1String("--log"))
            options.logPath = value;
        else if (a == QLatin1String("--secret-file"))
            options.secretPath = value;
        else
            return usage("unknown option");
    }
    if (options.socketPath.isEmpty() || options.statePath.isEmpty())
        return usage("XDG_RUNTIME_DIR is not set");
    options.ownLoopExe = ownLoopExecutable();

    QString error;
    const QByteArray secret = loadOrCreateSecret(options.secretPath, &error);
    if (secret.size() != 32) {
        std::fprintf(stderr, "riley-agentd: %s\n", qPrintable(error));
        return 1;
    }
    if (!privateFolder(QFileInfo(options.socketPath).absolutePath(), &error)) {
        std::fprintf(stderr, "riley-agentd: %s\n", qPrintable(error));
        return 1;
    }

    Service service(options, secret);
    service.setConfig(loadConfigFile(options.configPath));
    service.writeIdle();
    if (!service.openMouse(&error))
        std::fprintf(stderr, "riley-agentd: %s (click will fail)\n", qPrintable(error));

    // Reloaded on every change; an invalid file means no permission at all until it is fixed.
    QFileSystemWatcher watcher;
    QTimer reload;
    reload.setSingleShot(true);
    reload.setInterval(100);
    auto watch = [&] {
        if (!watcher.files().contains(options.configPath) && QFileInfo::exists(options.configPath))
            watcher.addPath(options.configPath);
        const QString dir = QFileInfo(options.configPath).absolutePath();
        if (!watcher.directories().contains(dir) && QFileInfo::exists(dir))
            watcher.addPath(dir);
    };
    watch();
    QObject::connect(&watcher, &QFileSystemWatcher::fileChanged, &reload, qOverload<>(&QTimer::start));
    QObject::connect(&watcher, &QFileSystemWatcher::directoryChanged, &reload, qOverload<>(&QTimer::start));
    QObject::connect(&reload, &QTimer::timeout, [&] {
        const ConfigLoad load = loadConfigFile(options.configPath);
        service.setConfig(load);
        if (load.status != ConfigStatus::Ok)
            std::fprintf(stderr, "riley-agentd: config refused, everything denied: %s\n", qPrintable(load.error));
        watch();
    });

    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, [&] { service.tick(); });
    tick.start(5000);

    const int quitFd = signalfd(-1, &quit, SFD_CLOEXEC | SFD_NONBLOCK);
    QSocketNotifier quitNotifier(quitFd, QSocketNotifier::Read);
    QObject::connect(&quitNotifier, &QSocketNotifier::activated, &app, &QCoreApplication::quit);

    const int listenFd = listenOn(options.socketPath, &error);
    if (listenFd < 0) {
        std::fprintf(stderr, "riley-agentd: %s\n", qPrintable(error));
        return 1;
    }
    std::thread([listenFd, &service] {
        for (;;) {
            const int fd = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR || errno == ECONNABORTED)
                    continue;
                return;
            }
            std::thread([fd, &service] { service.serve(fd); }).detach();
        }
    }).detach();

    const int rc = app.exec();
    ::unlink(QFile::encodeName(options.socketPath).constData());
    service.closeMouse();
    service.writeIdle();
    std::fflush(stderr);
    _exit(rc);
}
