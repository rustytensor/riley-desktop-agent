# riley-desktop-agent

Lets an agent work the Hyprland desktop (see windows, read their accessibility tree, point, click, type, manage
windows) under permissions the user sets in an XML file. It has its own agent loop that talks to any
OpenAI-compatible endpoint, local by default, and a command-line program so coding agents such as Claude Code and Pi
can use the same actions under the same permissions. A stop key revokes everything at once.

C++ and Qt. Work in progress.

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
