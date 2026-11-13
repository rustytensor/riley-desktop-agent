#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installs a build of riley-desktop-agent. It builds nothing: build first (see README.md).
#
#   tools/install.sh --build BUILD [--prefix PREFIX] [--config-dir DIR] [--unit-dir DIR] [--start]
#
#   --build BUILD     the CMake build folder to install from (required)
#   --prefix PREFIX   default ~/.local: the four programs go to PREFIX/bin, the starter config to
#                     PREFIX/share/riley-desktop-agent/starter-config.xml
#   --config-dir DIR  default ~/.config/riley-desktop-agent: config.xml is written there from the starter config
#                     only when no file is there; a file that is there is never changed
#   --unit-dir DIR    default ~/.config/systemd/user: where riley-agentd.service is written
#   --start           systemctl --user daemon-reload, enable riley-agentd, then restart it so the new programs run.
#                     Without it no systemctl command is run at all.
#
# The programs are copied byte for byte (no strip, no RPATH change). Nothing else goes into PREFIX/bin. Every step
# it takes is printed, one line each. tools/uninstall.sh undoes it but keeps the config and the log.

set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCHEMA="$REPO/schema/config.xsd"
UNIT=riley-agentd.service

usage() {
    echo "usage: $0 --build BUILD [--prefix PREFIX] [--config-dir DIR] [--unit-dir DIR] [--start]" >&2
    exit 2
}
die() { echo "install: $*" >&2; exit 1; }

BUILD="" PREFIX="$HOME/.local" CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/riley-desktop-agent"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user" START=0
while [ $# -gt 0 ]; do
    case "$1" in
        --build) [ $# -ge 2 ] || usage; BUILD="$2"; shift 2 ;;
        --prefix) [ $# -ge 2 ] || usage; PREFIX="$2"; shift 2 ;;
        --config-dir) [ $# -ge 2 ] || usage; CONFIG_DIR="$2"; shift 2 ;;
        --unit-dir) [ $# -ge 2 ] || usage; UNIT_DIR="$2"; shift 2 ;;
        --start) START=1; shift ;;
        -h|--help) sed -n '3,18p' "$0"; exit 0 ;;
        *) usage ;;
    esac
done
[ -n "$BUILD" ] || usage
abspath() { case "$1" in /*) printf '%s\n' "$1" ;; *) printf '%s/%s\n' "$PWD" "$1" ;; esac; }
BUILD="$(abspath "$BUILD")" PREFIX="$(abspath "$PREFIX")"
CONFIG_DIR="$(abspath "$CONFIG_DIR")" UNIT_DIR="$(abspath "$UNIT_DIR")"

# Everything is checked before anything is written.
PROGRAMS=(src/agentd/riley-agentd src/cli/riley-agent src/loop/riley-agent-loop src/loop/riley-agent-window)
for p in "${PROGRAMS[@]}"; do
    [ -f "$BUILD/$p" ] && [ -x "$BUILD/$p" ] || die "$BUILD/$p is missing: build first"
done
STARTER="$BUILD/share/riley-desktop-agent/starter-config.xml"
[ -f "$STARTER" ] || die "$STARTER is missing: build first"
command -v xmllint >/dev/null || die "xmllint is needed to check the starter config"
xmllint --noout --schema "$SCHEMA" "$STARTER" 2>/dev/null || die "$STARTER is not valid to $SCHEMA"

# The programs, byte for byte. Each is copied next to its place and renamed over it, so a running riley-agentd
# keeps its old file until it is restarted.
install -d -m 0755 "$PREFIX/bin"
for p in "${PROGRAMS[@]}"; do
    name="${p##*/}"
    tmp="$PREFIX/bin/.$name.new"
    cp -- "$BUILD/$p" "$tmp"
    chmod 0755 "$tmp"
    mv -f -- "$tmp" "$PREFIX/bin/$name"
    echo "bin: $PREFIX/bin/$name"
done

install -d -m 0755 "$PREFIX/share/riley-desktop-agent"
install -m 0644 "$STARTER" "$PREFIX/share/riley-desktop-agent/starter-config.xml"
echo "starter: $PREFIX/share/riley-desktop-agent/starter-config.xml"

# config.xml: only when none is there. A dangling link counts as there.
CONFIG="$CONFIG_DIR/config.xml"
if [ -e "$CONFIG" ] || [ -L "$CONFIG" ]; then
    echo "config: kept $CONFIG (it was there; the starter config is at $PREFIX/share/riley-desktop-agent/starter-config.xml)"
else
    (umask 077 && mkdir -p -- "$CONFIG_DIR")
    chmod 00700 "$CONFIG_DIR"   # five digits: also clears a set-group-id bit inherited from the parent
    (umask 077 && set -o noclobber && cat -- "$STARTER" >"$CONFIG") || die "could not write $CONFIG"
    chmod 0600 "$CONFIG"
    echo "config: wrote $CONFIG from the starter config"
fi

# The user unit. It starts and stops with the graphical session.
install -d -m 0755 "$UNIT_DIR"
tmp="$UNIT_DIR/.$UNIT.new"
cat >"$tmp" <<EOF
# Written by riley-desktop-agent's tools/install.sh. tools/uninstall.sh removes it.
[Unit]
Description=riley-desktop-agent service: desktop actions under the permissions in config.xml
PartOf=graphical-session.target
After=graphical-session.target

[Service]
Type=simple
ExecStart=$PREFIX/bin/riley-agentd
Restart=on-failure
RestartSec=2

[Install]
WantedBy=graphical-session.target
EOF
chmod 0644 "$tmp"
mv -f -- "$tmp" "$UNIT_DIR/$UNIT"
echo "unit: $UNIT_DIR/$UNIT"

if [ "$START" -eq 1 ]; then
    systemctl --user daemon-reload
    systemctl --user enable "$UNIT" 2>&1 | sed 's/^/systemctl: /'
    systemctl --user restart "$UNIT"
    echo "service: $UNIT enabled and (re)started: $(systemctl --user is-active "$UNIT" || true)"
else
    echo "service: not started (no --start)"
fi
