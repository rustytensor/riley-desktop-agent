# riley-desktop-agent

Lets an agent work the Hyprland desktop (see windows, read their accessibility tree, point, click, type, manage
windows) under permissions the user sets in an XML file. It has its own agent loop that talks to any
OpenAI-compatible endpoint, local by default, and a command-line program so coding agents such as Claude Code and Pi
can use the same actions under the same permissions. A stop key revokes everything at once.

C++ and Qt. Work in progress.

## Install

Needs Qt 6.8 or later (Core, Gui, Network, Qml, Quick, QuickControls2, Test), libxml2, libatspi (atspi-2, with
GLib), CMake and Ninja; at run time Hyprland, `grim`, `wtype`, `notify-send`, `xmllint`, and read-write access
to `/dev/uinput`.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
tools/install.sh --build build --start
```

`tools/install.sh` builds nothing. It copies `riley-agentd`, `riley-agent`, `riley-agent-loop` and
`riley-agent-window` to `~/.local/bin`, the starter config to `~/.local/share/riley-desktop-agent/`, writes
`~/.config/riley-desktop-agent/config.xml` from the starter config only if there is no file there (it never
overwrites one), and writes the user unit `~/.config/systemd/user/riley-agentd.service`. `--start` enables and
(re)starts it; without it no `systemctl` command runs. `--prefix`, `--config-dir` and `--unit-dir` install
elsewhere. `tools/uninstall.sh` stops and disables the service and removes the programs and the unit; it keeps
`config.xml` and `log.xml`.

The starter config allows Claude Code and Pi to see and list the windows on monitor DP-2 and nothing else; the
tool's own loop is listed with a local endpoint and no permissions. Edit `config.xml` to change that. Bind a key
to `riley-agent stop` (`docs/riley.md` has the bind and the bar pill). Files and formats: `docs/files.md`; how
coding agents use it: `docs/agents.md`; the agent loop and its window: `docs/loop.md`.

## What the permissions can and cannot stop

The permissions bind fully the tool's own agent loop and any caller that goes through `riley-agent`. A coding agent
that has a shell runs as you and can go around `riley-agent`: type with `wtype`, make its own input device on
`/dev/uinput`, drive windows with `hyprctl dispatch`, read and press controls over AT-SPI, drop its process ancestry
with `systemd-run --user`, or delete the stop state. For a coding agent the permissions are a cooperative boundary,
unless it runs sandboxed without those paths. `tools/riley-sandbox` runs a command with bwrap so that `riley-agent`
is its only way to the desktop: no `/dev/uinput`, no Wayland, X or Hyprland socket, the tool's config and state
read-only. Details, and what the sandbox still leaves open, in `docs/agents.md`.

## Licence

GPL-3.0-or-later. See `LICENSE`.
