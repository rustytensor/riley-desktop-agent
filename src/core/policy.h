// SPDX-License-Identifier: GPL-3.0-or-later
//
// The permission engine. decide() is a pure function of the request, the loaded config and the state; it is the
// only place that says yes or no. The rules, their order and the reason codes are written out in
// tests/policy/cases.xml (R0 to R14) and summarised in docs/files.md.
#pragma once

#include "config.h"

#include <QString>
#include <QStringList>

namespace riley {

// The target window as the service sees it. For launch, windowClass is the app to launch.
struct WindowInfo {
    QString windowClass, title, monitor;
    QString agent;          // coding agent found in the window's process tree: claude-code, pi, codex or empty
    bool focused = false;   // the window has keyboard focus
};

struct Request {
    QString client;         // client kind as identified by the service
    QString action;
    WindowInfo window;
    bool passwordField = false;   // the focused control is a password field
    bool passwordUnknown = false; // AT-SPI could not tell whether the focused control is a password field
                                  // (the app is not on the accessibility bus, or it has no focused accessible)
    bool focusMoved = false;      // the active window was not the target at the re-check before the key event
    QString text;                 // type: the text; key: the key name
};
// The action "resume" is decided here too (R0), by who asks alone: stop and segfault are never refused and are not
// decided by the engine.

struct State {
    bool stopped = false;   // stop, segfault or the stop key, not yet resumed
};

enum class Verdict { Allow, Deny };

struct Decision {
    Verdict verdict = Verdict::Deny;
    QString reason;   // a reason code, "allowed" when allowed
    QString rule;     // the rule that decided: a permission id, a deny-always id, or the rule number (R0..R14)
};

// The actions the engine knows, in the order the help text lists them.
const QStringList &knownActions();

Decision decide(const Request &request, const ConfigLoad &config, const State &state);

// One window against one config pattern (class, title, monitor globs; an absent title or monitor matches anything).
bool windowMatches(const WindowPattern &pattern, const WindowInfo &window);

} // namespace riley
