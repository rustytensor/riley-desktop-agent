#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# M4 gate: the agent loop (riley-agent-loop), driven by a made-up OpenAI-compatible server, against the M2 test app
# on DP-2.
#
#   tests/run-loop.sh                    build and check the real binaries; must print PASS
#   tests/run-loop.sh --plant <fault>    build the planted fault into the -fault build dir; must print FAIL, with the
#                                        check that targets the fault among the failures
#
# The build runs first, outside the shared DP-2 lock, then the window checks without the desktop (below, (f)). The
# desktop phase then runs under ~/.local/bin/dp2-lock and ends itself after 5 minutes, with the M2 procedure
# (tests/desktop/gate.py, imported): notify, the test app and the decoy on an empty, unused DP-2 workspace, placement
# and visibility checks, then DP-2, Marc's windows, the focus and the cursor restored and checked, also on failure,
# SIGINT, SIGTERM and the cap. The loop runs offscreen with no Wayland, X or Hyprland access of its own: it reaches
# the desktop only through the gate's private riley-agentd, and its permissions name only the two test windows.
#
# Each case starts its own made-up server (tests/loop/mock_server.py, 127.0.0.1, a port the kernel picks) playing
# tests/loop/scripts/<case>.json and recording every request, its own config and its own private riley-agentd:
#   (a) a scripted task on a local endpoint: windows, see, two clicks, click the field, type: the app counts exactly
#       2 clicks and holds the exact text; the service log shows exactly that sequence from client own-loop.
#   (b) a remote endpoint: see the test app (on the remote allow-list) and the decoy (not on it): every picture in
#       every request is the test app's and none shows the decoy's colour; the refusal reaches the model as a tool
#       result naming remote-not-allowed.
#   (c) the safe word: the task "segfault" stops the service and ends the loop without any model request; segfault
#       sent on the loop's control path (its standard input) during slow typing ends the typing within 200 ms and no
#       model request follows.
#   (d) a denied action: typing into the password field is refused, the next request carries the refusal, and the
#       loop carries on.
#   (e) the endpoint key, from a file and from an environment variable (made-up keys), is sent as the Authorization
#       header and appears in no file the run wrote and not on the output.
#   (f) window resume (spec change, lead decision: the window is its own process, riley-agent-window, and starts
#       riley-agent-loop per task): in a bwrap sandbox like the M3 gate's (no desktop, no network, the gate's own
#       coding agent hidden from the service by a private process namespace), with a private riley-agentd stopped
#       first, a test-only harness (tests/loop/harness, never installed) builds the window as riley-agent-window does
#       and presses Re-enable through Qt Test: from a plain parent the service resumes (bar idle); under a parent
#       named claude it is refused resume-needs-user and the bar stays stopped; riley-agent resume under a parent
#       named riley-agent-loop is refused resume-needs-user; riley-agent-window --help, every option it names and
#       every --name string in its binary never resume. Also there: riley-agent-window --list-ui (offscreen) names
#       the task field, transcript view, stop and re-enable controls.
# Expected values, the reasoning per case (which rule decides) and the interface the loop must follow:
# tests/loop/gate.py, and for (f) tests/loop/window_gate.py. A live smoke run is not part of the gate: it prints
# SKIP when no local endpoint answers.
#
# Known planted faults: loop-image-leak (see cmake/planted-faults.cmake): the loop attaches a picture even when see
# was refused (it re-sends the last picture it kept). Caught by the remote-image checks of case (b).
# window-hidden-resume: riley-agent-window accepts an option --help does not name, which resumes at start. Caught by
# the f4 checks of case (f).
# Needs a Hyprland session with DP-2, and must run under Claude Code or Pi (as the M2 gate).

set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "${1:-}" in
    -h|--help) sed -n '3,49p' "$0"; exit 0 ;;
esac
exec python3 -I "$REPO/tests/loop/gate.py" "$@"
