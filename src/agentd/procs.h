// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who is asking, and which coding agent owns a window, from /proc.
#pragma once

#include <QString>

namespace riley {

// The client kind of a caller, from its process ancestry: the nearest ancestor (the caller included) that is a coding
// agent decides: a process named "claude" is claude-code, "pi" is pi, "codex" is codex (never a configured client);
// the tool's own loop is recognised by its executable (ownLoopExe). Empty when none is found.
QString callerKind(qint64 pid, const QString &ownLoopExe);

// The coding agent running inside a window: the window's process and its descendants (at most 4096 processes)
// are searched for claude, pi or codex. "claude-code", "pi", "codex" or empty.
QString windowAgent(qint64 pid);

} // namespace riley
