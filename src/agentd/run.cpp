// SPDX-License-Identifier: GPL-3.0-or-later
#include "run.h"

#include <QStandardPaths>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace riley {

namespace {

struct CStrings {
    std::vector<QByteArray> storage;
    std::vector<char *> pointers;
    explicit CStrings(const QStringList &list)
    {
        for (const QString &s : list)
            storage.push_back(s.toLocal8Bit());
        for (QByteArray &s : storage)
            pointers.push_back(s.data());
        pointers.push_back(nullptr);
    }
    char **get() { return pointers.data(); }
};

QString resolve(const QString &program)
{
    if (program.contains(QLatin1Char('/')))
        return program;
    return QStandardPaths::findExecutable(program);
}

} // namespace

QStringList ownEnvironment()
{
    QStringList env;
    for (char **e = environ; e && *e; ++e)
        env << QString::fromLocal8Bit(*e);
    return env;
}

RunResult runProgram(const QStringList &argv, int timeoutMs, const QStringList &env)
{
    RunResult result;
    if (argv.isEmpty())
        return result;
    const QString path = resolve(argv.first());
    if (path.isEmpty())
        return result;

    int outPipe[2], errPipe[2];
    if (pipe2(outPipe, O_CLOEXEC) != 0)
        return result;
    if (pipe2(errPipe, O_CLOEXEC) != 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        return result;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], 1);
    posix_spawn_file_actions_adddup2(&actions, errPipe[1], 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attr, &none);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    CStrings args(argv);
    CStrings envs(env.isEmpty() ? ownEnvironment() : env);
    const QByteArray nativePath = path.toLocal8Bit();
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, nativePath.constData(), &actions, &attr, args.get(), envs.get());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(outPipe[1]);
    close(errPipe[1]);
    if (rc != 0) {
        close(outPipe[0]);
        close(errPipe[0]);
        return result;
    }
    result.started = true;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    pollfd fds[2] = {{outPipe[0], POLLIN, 0}, {errPipe[0], POLLIN, 0}};
    int open = 2;
    char buffer[65536];
    while (open > 0) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline
                                                                                 - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            result.timedOut = true;
            break;
        }
        const int n = poll(fds, 2, int(left.count()));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            result.timedOut = n == 0;
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            const ssize_t got = read(fds[i].fd, buffer, sizeof buffer);
            if (got > 0) {
                QByteArray &into = i == 0 ? result.out : result.err;
                if (i == 0 || into.size() < 65536)
                    into.append(buffer, got);
            } else if (got == 0 || errno != EINTR) {
                close(fds[i].fd);
                fds[i].fd = -1;
                --open;
            }
        }
    }
    for (pollfd &f : fds)
        if (f.fd >= 0)
            close(f.fd);
    if (result.timedOut)
        kill(pid, SIGKILL);   // by its own PID
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

qint64 spawnDetached(const QStringList &argv, const QString &cwd, const QStringList &env)
{
    if (argv.isEmpty())
        return -1;
    const QString path = resolve(argv.first());
    if (path.isEmpty())
        return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    const QByteArray nativeCwd = cwd.toLocal8Bit();
    if (!cwd.isEmpty())
        posix_spawn_file_actions_addchdir_np(&actions, nativeCwd.constData());
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attr, &none);
    sigset_t defaults;
    sigfillset(&defaults);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    CStrings args(argv);
    CStrings envs(env);
    const QByteArray nativePath = path.toLocal8Bit();
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, nativePath.constData(), &actions, &attr, args.get(), envs.get());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0)
        return -1;
    std::thread([pid] {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
    return pid;
}

} // namespace riley
