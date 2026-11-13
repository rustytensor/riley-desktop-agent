// SPDX-License-Identifier: GPL-3.0-or-later
//
// The request log: ~/.local/state/riley-desktop-agent/log.xml, one <request/> element per line, every request
// written whether allowed or refused. Typed text never reaches the log: only its length and SHA-256.
// The format is described in docs/files.md.
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
    QByteArray textSha256;       // lower-case hex, empty when no text
};

// Fills a record from a request and its decision; the text is reduced to its length and SHA-256 here.
LogRecord makeLogRecord(const Request &request, const Decision &decision, const QString &result,
                        const QDateTime &time = QDateTime::currentDateTimeUtc());

// One line of XML, without the trailing newline. Control characters in values are replaced.
QString logLine(const LogRecord &record);

// ~/.local/state/riley-desktop-agent/log.xml (honours XDG_STATE_HOME).
QString defaultLogPath();

// Appends one line; creates the folder 0700 and the file 0600 when missing. False with *error set on failure.
bool appendLog(const QString &path, const LogRecord &record, QString *error = nullptr);

} // namespace riley
