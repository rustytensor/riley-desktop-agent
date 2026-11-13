// SPDX-License-Identifier: GPL-3.0-or-later
//
// The state file Riley's bar reads to show the pill: $XDG_RUNTIME_DIR/riley-desktop-agent/state.json.
// Written atomically (write to a temporary file, then rename). The format is described in docs/files.md.
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

namespace riley {

enum class BarActivity {
    Idle,       // no client can see or act: the bar shows nothing
    Watching,   // a client has a session and may see
    Acting,     // an action is in progress (the pill turns red)
    Stopped,    // stopped; nothing runs until resumed
};

struct BarState {
    BarActivity activity = BarActivity::Idle;
    QString client;         // client kind, empty when idle
    QString action;         // the action in progress, only while acting
    QDateTime since;        // when this state began
};

QString barActivityName(BarActivity activity);

// The file's contents: one JSON object followed by a newline.
QByteArray barStateJson(const BarState &state);

// $XDG_RUNTIME_DIR/riley-desktop-agent/state.json; empty when XDG_RUNTIME_DIR is not set.
QString defaultBarStatePath();

// Writes the file atomically; the folder is made 0700 and the file 0600. False with *error set on failure.
bool writeBarState(const QString &path, const BarState &state, QString *error = nullptr);

} // namespace riley
