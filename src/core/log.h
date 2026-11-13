// SPDX-License-Identifier: GPL-3.0-or-later
//
// The request log: ~/.local/state/riley-desktop-agent/log.xml, one <request/> element per line, every request
// written whether allowed or refused. Typed text never reaches the log: only its length and an HMAC-SHA-256 keyed
// with a per-install random secret (so a short text cannot be guessed back from the log). The format is described in
// docs/files.md.
#pragma once

#include "policy.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>

namespace riley {

struct LogRecord {
    QDateTime time;           // written in UTC
    QString client;
    QString action;
    WindowInfo window;
    Decision decision;
    QString result;           // what happened: refused, done, failed, stopped (free text from the service)
    qsizetype textLength = -1;   // -1: the request carried no text
    QByteArray textHmac;         // lower-case hex HMAC-SHA-256 of the UTF-8 text, empty when no text
};

// Fills a record from a request and its decision; the text is reduced to its length and its HMAC-SHA-256 keyed
// with `secret` here.
LogRecord makeLogRecord(const Request &request, const Decision &decision, const QString &result,
                        const QByteArray &secret, const QDateTime &time = QDateTime::currentDateTimeUtc());

// One line of XML, without the trailing newline. Control characters in values are replaced.
QString logLine(const LogRecord &record);

// A stop, segfault or resume: <control time=".." client=".." action=".." result=".."/>, one line.
QString controlLogLine(const QDateTime &time, const QString &client, const QString &action, const QString &result);

// ~/.local/state/riley-desktop-agent/hash-secret (honours XDG_STATE_HOME).
QString defaultSecretPath();

// The log's hash key: 32 random bytes. Created 0600 (folder 0700) when missing. Empty with *error set when the file
// cannot be read or created, or is not exactly 32 bytes, or is readable by anyone else.
QByteArray loadOrCreateSecret(const QString &path, QString *error = nullptr);

// ~/.local/state/riley-desktop-agent/log.xml (honours XDG_STATE_HOME).
QString defaultLogPath();

// ~/.local/state/riley-desktop-agent/stopped (honours XDG_STATE_HOME): there while a stop is in force, so a
// restarted service starts stopped. Only an allowed resume removes it.
QString defaultStopPath();

// Appends one line; creates the folder 0700 and the file 0600 when missing. False with *error set on failure.
bool appendLog(const QString &path, const LogRecord &record, QString *error = nullptr);
bool appendLogLine(const QString &path, const QString &line, QString *error = nullptr);

// What log.xml says about a stop, for the service's start-up when the persisted stop copy is missing.
enum class LogStop {
    None,      // no log yet, no stop in it, or the last stop was followed by a resume done
    Stopped,   // the last stop or segfault (result "done") has no resume done after it
    Unknown,   // the log cannot be read, or a line read before the answer was found is not one XML element
};
struct LogStopCheck {
    LogStop verdict = LogStop::Unknown;
    QString why;   // one plain sentence for the start-up log line
};

// Reads log.xml from the end, line by line, back to the last control line that decides: a stop or segfault with
// result "done" (stopped) or a resume with result "done" (not stopped). A refused resume and every other line are
// passed over. A cut-off or malformed line on the way gives Unknown, which the service treats as stopped.
LogStopCheck stopFromLog(const QString &path);

} // namespace riley
