#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# M5 gate: the installed build on Marc's desktop.
#
#   tests/run-install.sh                         build, install to ~/.local with the systemd user unit (enabled and
#                                                started), then checks 1-6; must print PASS
#   tests/run-install.sh --prefix STAGE/DIR      build and install into a staging folder under
#                                                /srv/devops/scratch/riley-desktop-agent/stage: no unit touched,
#                                                nothing under ~/.local or ~/.config; checks 1-3
#   tests/run-install.sh --plant <fault>         build the planted fault into the -fault build dir and install it
#                                                to the staging folder only; must print FAIL, caught by check 3
#
# Checks (expected values and the installer contract: tests/install/gate.py; the starter config's table and the
# reasoning per case: tests/install/starter-cases.xml):
#   1. builds into /srv/devops/forge/build/riley-desktop-agent (-fault for a planted fault), outside any lock.
#   2. tools/install.sh: the 4 binaries at PREFIX/bin, executable, sha256 equal to the build's; no test binary; the
#      starter copy valid to the XSD; config.xml written only if none was there, never overwritten (also checked
#      with a made-up existing file in staging); the unit file passes systemd-analyze --user verify, PartOf/After/
#      WantedBy graphical-session.target, Restart=on-failure, ExecStart PREFIX/bin/riley-agentd, no inhibitor.
#      Real mode: riley-agentd active and enabled, running the installed binary; socket 0600 in a 0700 folder;
#      state.json. Staging: no systemctl call, the user's units and files unchanged.
#   3. the starter config through the M1 driver: at least 20 hand-worked cases (see and windows on DP-2 allowed for
#      claude-code and pi; off DP-2 refused; every other action refused; the own loop refused; deny-always wins;
#      unknown clients refused), and the config's shape as the plan says.
#   4. (real) `~/.local/bin/riley-agent windows` from this session lists only DP-2 windows; read-only.
#   5. (real) run-policy.sh, run-cli.sh, run-desktop.sh, run-loop.sh once each with --installed ~/.local.
#   6. (real) the installed service survived them untouched.
#
# Known planted faults (see cmake/planted-faults.cmake):
#   starter-config-too-wide  the installer's starter config also allows pi click on DP-2. Caught by check 3
#                            (case s20, targets="starter-config-too-wide").
# Must run under Claude Code or Pi (check 4: the service identifies the caller by process ancestry).
# Output: /srv/devops/scratch/riley-desktop-agent/install-gate/.

set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "${1:-}" in
    -h|--help) sed -n '3,36p' "$0"; exit 0 ;;
esac
exec python3 -I "$REPO/tests/install/gate.py" "$@"
