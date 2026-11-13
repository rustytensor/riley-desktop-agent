#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# M3 gate: the command line for coding agents (riley-agent), without the real desktop.
#
#   tests/run-cli.sh                    build and check the real binaries; must print PASS
#   tests/run-cli.sh --plant <fault>    build the planted fault into the -fault build dir; must print FAIL, with
#                                       every call that targets the fault among the failures
#
# The build runs first. Then tests/cli/gate.py runs inside a bwrap sandbox: its own process, network, IPC and UTS
# namespaces; a fresh /dev (no /dev/uinput); empty tmpfs over /tmp, /home and $XDG_RUNTIME_DIR (no Wayland, X,
# session bus or real Hyprland socket; Marc's home and state are not visible); a clean environment. The repo and the
# build dir are read-only inside; only the gate's scratch folder is writable. The gate serves a fake Hyprland IPC
# socket (j/monitors, j/clients, j/activewindow from a scripted desk; every dispatch answered with an error), so an
# allowed action fails harmlessly after its decision and nothing reaches Marc's desktop, cursor or DP-2 (no
# dp2-lock needed). It asserts all of that before it starts anything.
#
# Checks (cases, expected results and the reasoning per case: tests/cli/cases.xml):
#   1. every subcommand (see tree windows point click type key launch close stop segfault resume help) called as
#      Claude Code and as Pi (riley-agent run as the child of a launcher copy named claude / pi): every M1 case for
#      claude-code or pi that a fake window and CLI arguments can express gives the M1 decision: allow is the
#      service log's decision="allow" with the permission id (the action then fails in the sandbox: "failed",
#      exit 4); deny is "refused <reason>", exit 1. The table is checked against tests/policy/cases.xml itself.
#   2. a caller with no recognised client (plain shell, no agent among its ancestors) is refused every action
#      subcommand with unknown-client; stop and segfault work for anyone; resume works only for that caller.
#   3. a client kind the config does not list (fixture tests/cli/fixtures/cc-only.xml) is refused when called as Pi.
#   4. riley-agent help: under 60 lines, names every subcommand, plain text, does not reach the service.
#   5. every call: plain text, one result (first line ok / refused <reason> / stopped / failed <what>, with the
#      matching exit status 0 / 1 / 3 / 4), one service log line, no Hyprland dispatch after a refusal.
#   6. the stop survives a restart: stop, kill the service by PID (SIGKILL), start it with the same state folder:
#      still stopped (requests refused, bar state "stopped"); a resume from Claude Code is refused; a resume from
#      the plain shell clears it.
#
# Known planted faults: unlisted-client-accepted (see cmake/planted-faults.cmake): a caller with no recognised
# client is treated as the tool's own loop. Caught by the calls of check 2 (targets="unlisted-client-accepted").
#
# Output of the calls and the services: /srv/devops/scratch/riley-desktop-agent/cli-gate/.

set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRATCH=/srv/devops/scratch/riley-desktop-agent/cli-gate
BUILD=/srv/devops/forge/build/riley-desktop-agent
PLANT=""
KNOWN="unlisted-client-accepted"

while [ $# -gt 0 ]; do
    case "$1" in
        --plant) PLANT="${2:-}"; shift 2 || { echo "usage: $0 [--plant <fault>]"; exit 2; } ;;
        -h|--help) sed -n '3,38p' "$0"; exit 0 ;;
        *) echo "usage: $0 [--plant <fault>]"; exit 2 ;;
    esac
done
case "$PLANT" in
    ""|unlisted-client-accepted) ;;
    *) echo "unknown planted fault: $PLANT (known: $KNOWN)"; exit 2 ;;
esac
[ -n "$PLANT" ] && BUILD=/srv/devops/forge/build/riley-desktop-agent-fault

mkdir -p "$SCRATCH"
rm -f "$SCRATCH"/*.log "$SCRATCH"/calls.txt

fail_early() {
    echo "FAIL $*"
    [ -n "$PLANT" ] && echo "PLANTED FAULT $PLANT NOT CAUGHT (the gate did not run)"
    echo "FAIL (1 checks failed)"
    exit 1
}

# Build outside the sandbox (the planted fault only ever into the -fault build dir).
if ! cmake -S "$REPO" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRILEY_PLANTED_FAULT="$PLANT" \
        >"$SCRATCH/configure.out" 2>&1; then
    fail_early "build: cmake configure failed (see $SCRATCH/configure.out)"
fi
if ! cmake --build "$BUILD" --target riley-agentd riley-agent -j8 >"$SCRATCH/build.out" 2>&1; then
    fail_early "build: riley-agentd or riley-agent did not build (see $SCRATCH/build.out)"
fi
echo "PASS build: riley-agentd, riley-agent${PLANT:+ with planted fault $PLANT}"

command -v bwrap >/dev/null || fail_early "sandbox: bwrap is not installed"
command -v xmllint >/dev/null || fail_early "xmllint is not installed"
if xmllint --noout --schema "$REPO/schema/config.xsd" "$REPO/tests/cli/fixtures/cc-only.xml" >"$SCRATCH/xmllint.out" 2>&1; then
    echo "PASS schema accepts tests/cli/fixtures/cc-only.xml"
else
    fail_early "schema accepts tests/cli/fixtures/cc-only.xml: $(grep -m1 -i error "$SCRATCH/xmllint.out")"
fi

RUNTIME="/run/user/$(id -u)"
bwrap --die-with-parent --new-session \
    --unshare-pid --unshare-net --unshare-ipc --unshare-uts --unshare-cgroup-try \
    --ro-bind / / --dev /dev --proc /proc \
    --tmpfs /tmp --tmpfs /home --perms 0700 --tmpfs "$RUNTIME" \
    --ro-bind "$REPO" "$REPO" --ro-bind "$BUILD" "$BUILD" --bind "$SCRATCH" "$SCRATCH" \
    --clearenv --setenv PATH /usr/bin:/bin --setenv LANG C.UTF-8 --setenv HOME /tmp/riley-cli-gate \
    --setenv XDG_RUNTIME_DIR "$RUNTIME" --setenv HYPRLAND_INSTANCE_SIGNATURE riley-cli-gate-fake-hyprland \
    --chdir /tmp \
    python3 -I "$REPO/tests/cli/gate.py" --repo "$REPO" --build "$BUILD" --out "$SCRATCH" --plant "$PLANT"
exit $?
