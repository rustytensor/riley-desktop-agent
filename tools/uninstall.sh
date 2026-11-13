#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Undoes tools/install.sh: stops and disables riley-agentd, removes the four programs, the starter copy and the
# unit file. It never removes config.xml, log.xml, the hash key or the stop file.
#
#   tools/uninstall.sh [--prefix PREFIX] [--unit-dir DIR] [--no-systemctl]
#
#   --prefix PREFIX   default ~/.local (as given to tools/install.sh)
#   --unit-dir DIR    default ~/.config/systemd/user
#   --no-systemctl    run no systemctl command (for an install into a test folder)

set -euo pipefail
UNIT=riley-agentd.service
PREFIX="$HOME/.local" UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user" SYSTEMCTL=1
usage() { echo "usage: $0 [--prefix PREFIX] [--unit-dir DIR] [--no-systemctl]" >&2; exit 2; }
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) [ $# -ge 2 ] || usage; PREFIX="$2"; shift 2 ;;
        --unit-dir) [ $# -ge 2 ] || usage; UNIT_DIR="$2"; shift 2 ;;
        --no-systemctl) SYSTEMCTL=0; shift ;;
        -h|--help) sed -n '3,12p' "$0"; exit 0 ;;
        *) usage ;;
    esac
done

if [ "$SYSTEMCTL" -eq 1 ]; then
    systemctl --user disable --now "$UNIT" 2>&1 | sed 's/^/systemctl: /' || true
    echo "service: $UNIT stopped and disabled"
fi
for name in riley-agentd riley-agent riley-agent-loop riley-agent-window; do
    if [ -e "$PREFIX/bin/$name" ]; then rm -f -- "$PREFIX/bin/$name"; echo "removed: $PREFIX/bin/$name"; fi
done
if [ -e "$PREFIX/share/riley-desktop-agent/starter-config.xml" ]; then
    rm -f -- "$PREFIX/share/riley-desktop-agent/starter-config.xml"
    rmdir -- "$PREFIX/share/riley-desktop-agent" 2>/dev/null || true
    echo "removed: $PREFIX/share/riley-desktop-agent/starter-config.xml"
fi
if [ -e "$UNIT_DIR/$UNIT" ]; then
    rm -f -- "$UNIT_DIR/$UNIT"
    echo "removed: $UNIT_DIR/$UNIT"
    [ "$SYSTEMCTL" -eq 1 ] && systemctl --user daemon-reload
fi
echo "kept: config.xml, log.xml, the hash key and the stop file (remove them by hand if you want them gone)"
