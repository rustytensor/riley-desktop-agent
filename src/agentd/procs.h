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

// For resume only (rule R0): true when the caller or one of its ancestors is named like the tool's own loop
// (comm "riley-agent-loo", riley-agent-loop cut to the kernel's 15 characters). It only widens the refusal of a
// resume; it never makes a caller own-loop, which takes the loop's executable (callerKind). Also true when an
// ancestor cannot be read: a resume then fails closed.
bool loopNamedAncestor(qint64 pid);

// The coding agent running inside a window: the window's process and its descendants (at most 4096 processes)
// are searched for claude, pi or codex. "claude-code", "pi", "codex" or empty.
QString windowAgent(qint64 pid);

} // namespace riley
