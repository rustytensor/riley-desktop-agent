#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Check 8 probe (tests/run-cli.sh): run by tests/cli/gate.py inside its decoy-host sandbox, once through
# tools/riley-sandbox and once without it. It reports what it can reach, one "probe NAME VALUE" line each, tries to
# change the tool's config and state (the gate looks at the result from outside), and calls riley-agent windows.
#
#   sandbox-probe.sh RUNTIME CONFIG STATE_DIR AGENT HOME_MARKER
#     RUNTIME      the XDG_RUNTIME_DIR the gate's service uses (checked as well as the probe's own XDG_RUNTIME_DIR)
#     CONFIG       the service's config.xml
#     STATE_DIR    the service's state folder (log.xml, the stop copy)
#     AGENT        the riley-agent binary
#     HOME_MARKER  a file the gate wrote in $HOME
RT="$1"; CONF="$2"; STATE="$3"; AGENT="$4"; MARKER="$5"

seen() { if [ -e "$1" ] || [ -S "$1" ]; then echo present; else echo absent; fi; }
any_wayland() {
    for d in "$RT" "${XDG_RUNTIME_DIR:-}"; do
        [ -n "$d" ] || continue
        for f in "$d"/wayland-*; do
            if [ -e "$f" ] || [ -S "$f" ]; then echo present; return; fi
        done
    done
    echo absent
}
any_hypr() {
    for d in "$RT" "${XDG_RUNTIME_DIR:-}"; do
        if [ -n "$d" ] && [ -e "$d/hypr" ]; then echo present; return; fi
    done
    echo absent
}

echo "probe uinput $(seen /dev/uinput)"
echo "probe wayland-socket $(any_wayland)"
if [ -n "${WAYLAND_DISPLAY:-}" ]; then echo "probe wayland-display set"; else echo "probe wayland-display unset"; fi
echo "probe x-socket $(seen /tmp/.X11-unix/X0)"
if [ -n "${DISPLAY:-}" ]; then echo "probe x-display set"; else echo "probe x-display unset"; fi
echo "probe hypr-dir $(any_hypr)"
echo "probe agent-socket $(seen "$RT/riley-desktop-agent/agentd.sock")"
echo "probe home-marker $(seen "$MARKER")"
if (true > "./probe-project-write") 2>/dev/null; then echo "probe project-write written"; else echo "probe project-write refused"; fi
echo "probe cwd $(pwd)"

# Attempts the gate judges from outside: append to the config, create a file in the state folder. Each in a
# subshell, so a refused write (read-only, missing) ends only that subshell, not the probe.
(printf '<!-- sandbox probe -->\n' >> "$CONF") 2>/dev/null
(true > "$STATE/probe-was-here") 2>/dev/null

out="$("$AGENT" windows 2>/dev/null)"
rc=$?
echo "agent-exit $rc"
printf '%s\n' "$out" | while IFS= read -r line; do echo "agent-out $line"; done
exit 0
