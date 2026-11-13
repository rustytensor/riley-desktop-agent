# riley-desktop-agent

Lets an agent work the Hyprland desktop (see windows, read their accessibility tree, point, click, type, manage
windows) under permissions the user sets in an XML file. It has its own agent loop that talks to any
OpenAI-compatible endpoint, local by default, and a command-line program so coding agents such as Claude Code and Pi
can use the same actions under the same permissions. A stop key revokes everything at once.

C++ and Qt. Work in progress.

## Licence

GPL-3.0-or-later. See `LICENSE`.
