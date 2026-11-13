# For Riley (the lead adds these)

Two things on Riley's side make the tool safe to use day to day: a pill on the bar that shows when an agent can see
or act, and the stop key. riley-desktop-agent does not edit Riley's bar, keys or Hyprland config; these are the
pieces to add there.

## The bar pill

Read `$XDG_RUNTIME_DIR/riley-desktop-agent/state.json` (format in `docs/files.md`, "state.json"). It is one line
of JSON:

```json
{"version":1,"state":"acting","client":"claude-code","action":"click","since":"2026-11-12T21:30:05.120Z"}
```

| `state` | Pill |
|---|---|
| `idle` | hidden |
| `watching` | shown, the client's name (`claude-code` as "Claude Code", `pi` as "Pi", `own-loop` as "Agent"), the normal pill colour: a session is open and the client may see |
| `acting` | shown, the client's name and the action (`Claude Code: click`), **red** |
| `stopped` | shown, "Agent stopped", a colour of its own that is not red (amber, for example): every permission is revoked until resumed. `client` is who stopped it |

- File missing, empty, not JSON, or `version` other than 1: hide the pill (the service is not running, or is a
  version the bar does not know). Do not treat it as `stopped`.
- `since` is UTC; a tooltip can show how long the state has lasted.
- A click on the stopped pill may open `~/.local/bin/riley-agent-window`, where the user can re-enable. The bar must
  never run `riley-agent resume` itself: resume is for the user's own hand.

When to read it again: the service replaces the file atomically (writes a temporary file in the same folder and
renames it over `state.json`), so watch the **folder** `$XDG_RUNTIME_DIR/riley-desktop-agent` with inotify for
`IN_MOVED_TO` and `IN_CLOSE_WRITE` on the name `state.json` (a watch on the file itself is lost at the first
rename). If the bar cannot use inotify, poll every 500 ms: `acting` must show while the action runs, and most
actions take under a second. The folder may not exist before the service first starts: watch
`$XDG_RUNTIME_DIR` for it, or retry every few seconds.

The service also sends one notification (`notify-send`, app name `riley-desktop-agent`) when a client's session
starts; the bar needs nothing for that.

## The stop key: Super+Shift+Escape

`riley-agent stop` revokes every permission and ends any action in progress within 200 ms; nothing resumes until
the user resumes in the tool's window or with `riley-agent resume` from their own terminal. The tool never reads
the keyboard itself, so the key is a compositor bind. In the form of Riley's `hypr/keys.lua` (its `bind(keys,
description, dispatcher)` helper runs a string through `hl.dsp.exec_cmd`):

```lua
bind(mod .. " + SHIFT + Escape",    "Desktop agent: stop (revoke everything)", os.getenv("HOME") .. "/.local/bin/riley-agent stop")
```

The same in the classic hyprland.conf form, for reference:

```
bind = SUPER SHIFT, Escape, exec, ~/.local/bin/riley-agent stop
```

`riley-agent stop` from the bind runs with no coding agent among its ancestors, so it is the user's own stop; it
works with or without a config and while the service is busy.
