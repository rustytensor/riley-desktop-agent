#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# M2 gate: the service (riley-agentd) and its actions, on DP-2, against a test app the gate starts itself.
#
#   tests/run-desktop.sh                    build and check the real service; must print PASS
#   tests/run-desktop.sh --plant <fault>    build the planted fault into the -fault build dir; must print FAIL,
#                                           with the check that targets the fault among the failures
#   tests/run-desktop.sh --check-placement  build only the test app, open it and the decoy on an empty DP-2 workspace,
#                                           read their AT-SPI roles, restore DP-2 (no service, no click)
#
# The build runs first, outside the shared DP-2 lock; the desktop phase then runs under ~/.local/bin/dp2-lock and
# ends itself after 5 minutes. It shows an empty, unused workspace on DP-2 for its windows (opened straight onto it
# with exec_cmd rules), and switches DP-2 back to the workspace it showed before, also on failure, SIGINT or SIGTERM;
# it checks Marc's window there kept its address, size and fullscreen state, and restores focus and the cursor.
#
# Checks (expected values, the reasoning per case and the safety rules: tests/desktop/gate.py):
#   clicks counted; typed text equal to what was sent; the password field refused (password-field); typing
#   refused when focus moves before a key event (focus-moved); see returns the test app's pixels (size and a known
#   colour block); tree finds the controls by role and name; stop and segfault end an action within 200 ms, and
#   everything is refused while stopped; resume from a coding agent is refused (resume-needs-user) and the stop
#   survives it, so the gate restarts its private service with a fresh state; the bar state file (idle, acting with
#   the client, stopped); one log line per request with a keyed hash and no typed text, and the control lines; DP-2,
#   Marc's windows, the focus and the cursor as before. Every process the gate started ended by its PID.
#
# --installed PREFIX (M5): the service and the CLI from PREFIX/bin instead of the build dir (the test app is
# always the build's); still its own private service instances, separate from the installed unit.
# Known planted faults: focus-recheck-removed (see cmake/planted-faults.cmake), caught by the focus-moved check.
# Needs a Hyprland session with DP-2, and must run under Claude Code or Pi (the service identifies the gate's
# requests by process ancestry).

set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "${1:-}" in
    -h|--help) sed -n '3,32p' "$0"; exit 0 ;;
esac
exec python3 -I "$REPO/tests/desktop/gate.py" "$@"
