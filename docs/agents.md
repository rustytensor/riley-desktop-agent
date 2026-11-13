# Coding agents

How Claude Code and Pi use riley-desktop-agent: through the `riley-agent` command line, called from their Bash
tool. There is no MCP server: a short help text costs far less context than a tool list, and the command line
gives the same decisions as any other way in.

## Setting it up

1. Install (README.md, "Install"): `riley-agentd` runs as a user service started with the graphical session (it
   owns the desktop actions; `riley-agent` only talks to it), and `riley-agent` is in `~/.local/bin`.
2. `~/.config/riley-desktop-agent/config.xml` lists the agent as a client and gives it permissions
   (`docs/files.md` has the format; `examples/config.xml` a commented example). Everything not allowed there is
   refused. The installer writes the starter config there when there is none: Claude Code and Pi may `see` and
   list (`windows`) the windows on DP-2, as remote clients, and nothing else (`docs/files.md`, "The starter
   config"). Widen it by hand, one permission at a time; the service reloads the file on change.
3. Tell the agent the tool exists, for example in the project's instructions:
   "To see or work the desktop, run `riley-agent help` once and use `riley-agent` through Bash."
4. Better: start the agent inside `tools/riley-sandbox` (below), so that `riley-agent` is its only way to the
   desktop.

## Who is calling

The service tells callers apart by their process ancestry, read from `/proc` with the caller's pid from the
socket (SO_PEERCRED): a process named `claude` among the caller's ancestors makes it `claude-code`, a process
named `pi` makes it `pi`. Nothing the caller says about itself counts. A caller with neither (and not the tool's
own loop) is unknown and refused everything but `stop`, `segfault` and `resume`. Codex is recognised but cannot be
given permissions: it is always refused.

A client kind the config does not list is refused (`unknown-client`), even when another kind is listed.

## Local or remote

Claude Code sends what it sees (pictures, window text, the accessibility tree) to its provider, so it counts as
**remote** unless its `<client>` entry says it runs a local model (`location="local"`, or an `endpoint` that is
local). Pi is remote or local by the model it uses: give its entry the endpoint or location that matches. A client
entry with neither a location nor an endpoint counts as remote.

A remote client may see and act on only the windows on the `<remote-allow>` list, whatever its permissions say
(`remote-not-allowed`). `riley-agent windows` lists only the windows the caller may see: each window is decided on
its own, the remote allow-list included, and the others are left out without a word.

## The help text

`riley-agent help` is all an agent needs to read. It prints:

```
riley-agent: see and act on the Hyprland desktop through riley-agentd, as config.xml allows.
One call, one result. Plain text out. Every call is checked and logged by the service.

Commands:
  windows                      list the windows you may see, one per line:
                               ADDR class monitor x,y wxh "title"
  see    -w ADDR -p FILE       a PNG of the window, written to FILE; FILE must be in a 0700 folder
                               under $XDG_RUNTIME_DIR (never in a project folder; delete it after use)
  tree   -w ADDR               the window's controls from the accessibility tree, "role: name",
                               indented two spaces per level
  point  -w ADDR X Y           move the pointer to X,Y in the window
  click  -w ADDR X Y [-b left|right|middle]   click at X,Y in the window (left by default)
  type   -w ADDR [-d MS] TEXT  type TEXT into the window's focused control (quote it; MS between keys)
  key    -w ADDR NAME          press one key: Return, Tab, Escape, BackSpace, ctrl+a, ctrl+shift+t
  launch APP                   start a program from /usr/bin, in your home folder, clean environment
  close  -w ADDR               ask the window to close
  stop                         revoke every permission and end what is running, at once
  segfault                     the same as stop (the safe word)
  resume                       end a stop; refused from a coding agent, only the user may resume
  help                         this text

Windows: run `riley-agent windows` first and use the window's address (ADDR, 0x...) with -w.
Coordinates are logical pixels inside the window: 0,0 is its top-left corner, wxh its size.
Typing goes only to the window that has focus; click into the field first. type and key are
refused for password fields, for fields the service cannot check, and for text that looks like
a key, token or card number. Never type passwords or keys.

Result: the first line is one of
  ok ...                      done (exit 0)
  refused REASON (rule R)     not allowed; REASON says why, R is the rule that decided (exit 1)
  stopped ...                 a stop is in force or came during the action (exit 3)
  failed WHAT: ...            allowed, but it did not work (exit 4)
Exit 2: bad usage, or riley-agentd is not running.

Common refusals: unknown-client (the config does not list you), no-permission (nothing allows this
action on this window), remote-not-allowed (you count as remote: you may only see and act on
windows on the remote allow-list), deny-* (never allowed: lock screen, password managers,
banking and payment, system settings, agent terminals, this tool), stopped, no-config.
A refusal is final for that request: do not retry it or work around it; tell the user.

Options: -w/--window ADDR, -p/--png FILE, -b/--button NAME, -d/--delay-ms MS, --socket PATH.
```

A typical session:

```
$ riley-agent windows
ok 2 windows
0x55d0c8a1e2f0 org.riley.testapp DP-2 1500,1900 800x600 "Riley test app"
0x55d0c8a40a10 firefox DP-2 2400,1900 1200x900 "Qt Documentation"
$ install -d -m 0700 "$XDG_RUNTIME_DIR/shots"
$ riley-agent see -w 0x55d0c8a1e2f0 -p "$XDG_RUNTIME_DIR/shots/app.png"
ok picture of 800x600 logical pixels, written to /run/user/1000/shots/app.png
$ riley-agent click -w 0x55d0c8a1e2f0 120 80
ok clicked left at 120,80 in the window
$ riley-agent type -w 0x55d0c8a1e2f0 "hello"
ok typed 5 key events
$ rm "$XDG_RUNTIME_DIR/shots/app.png"
```

## What the permissions can and cannot stop

The permissions bind fully only the tool's own loop and callers that go through `riley-agent`. A coding agent with a
shell runs as you, so it can go around `riley-agent` entirely:

- type into any window with `wtype`, or open `/dev/uinput` and make a virtual mouse or keyboard of its own;
- move windows, focus them or run commands with `hyprctl dispatch` over Hyprland's socket;
- read and press any app's controls over AT-SPI, for example from Python;
- take screenshots with `grim`;
- start a program with `systemd-run --user`, which drops its process ancestry, so the service no longer sees it as
  Claude Code or Pi (a `resume` from there is not refused);
- delete the stop copy, edit `log.xml` or `config.xml`, or end the service.

So for a coding agent the permissions are a cooperative boundary: they keep an agent that uses `riley-agent` inside
what you allowed, and they keep it from undoing a stop by mistake. They are not a wall against an agent that
decides to go around them, unless the agent runs sandboxed without those paths.

**Run the coding agent sandboxed**, so that `riley-agent` is its only way to the desktop. `tools/riley-sandbox`
does that with bwrap (no root needed):

```sh
cd ~/git/project && ~/git/riley-desktop-agent/tools/riley-sandbox --rw ~/.claude --rw ~/.claude.json claude
cd ~/git/project && ~/git/riley-desktop-agent/tools/riley-sandbox pi
```

Inside, the system and `$HOME` are read-only and the current folder (the project) is read-write; each `--rw PATH`
is read-write as well (Claude Code keeps its settings and sessions in `~/.claude` and `~/.claude.json`). There is
no `/dev/uinput`, an empty `/tmp` (no X socket), an empty `$XDG_RUNTIME_DIR` (no Wayland socket, no
`$XDG_RUNTIME_DIR/hypr`, no session bus, so no `systemd-run --user`), and `WAYLAND_DISPLAY` and `DISPLAY` are unset.
Only the `riley-agent` socket folder is bound back in. The tool's config and state folders (`config.xml`, the stop
copy, `log.xml`) and Hyprland's config folder stay read-only even under a `--rw`. The agent gets its own process
namespace. The core of it is:

```sh
bwrap --die-with-parent --unshare-pid --unshare-ipc \
    --ro-bind / / --dev /dev --proc /proc \
    --tmpfs /tmp --perms 0700 --tmpfs "$XDG_RUNTIME_DIR" \
    --ro-bind "$XDG_RUNTIME_DIR/riley-desktop-agent" "$XDG_RUNTIME_DIR/riley-desktop-agent" \
    --bind "$PWD" "$PWD" \
    --ro-bind ~/.config/riley-desktop-agent ~/.config/riley-desktop-agent \
    --ro-bind ~/.local/state/riley-desktop-agent ~/.local/state/riley-desktop-agent \
    --unsetenv WAYLAND_DISPLAY --unsetenv DISPLAY --chdir "$PWD" -- claude
```

What the sandbox leaves open: the network stays on, since the agent needs its provider, and with it Xwayland's
abstract socket, which a program that looks for it can still reach (add `--unshare-net` when the agent works with a
local model only). Anything you add with `--rw` can be changed by the agent, and a file it changes there (its own
settings, for example) is read again by the next session you start without the sandbox. Files in the project are
yours to review as with any agent.

Claude Code also has a sandbox of its own for the commands it runs; see its documentation on sandboxing. Before
relying on it instead of `tools/riley-sandbox`, check that it leaves out `/dev/uinput`, the Wayland socket and
`$XDG_RUNTIME_DIR/hypr`.

## What is refused, and why

The rule in a refusal is the rule of the permission engine that decided (`docs/files.md`, "How a request is
decided"); every request is written to the log, allowed or refused.

| Reason | Rule | Why |
|---|---|---|
| `resume-needs-user` | R0 | only the user may end a stop, from their own terminal or the tool's window; a coding agent or the tool's own loop cannot undo a stop |
| `no-config`, `invalid-config` | R1, R2 | no valid config: nothing is allowed |
| `stopped` | R3 | a stop is in force (it survives a restart of the service) until the user resumes |
| `unknown-client` | R4 | the caller is not a client the config lists |
| `unknown-action` | R5 | not an action the tool knows |
| `deny-lock-screen`, `deny-own-tool`, `deny-agent-terminal`, `deny-password-manager`, `deny-finance`, `deny-system-settings` | R6 | never allowed, whatever the config says: the lock screen; this tool's window and config; terminals running Claude Code, Pi or Codex (an agent must not drive another agent's session); password managers; banking, payment and crypto; system settings |
| `deny-config-list` | the entry's id | on the config's own deny-always list |
| `deny-agent-hook-editor` | R8 | an editor that loads coding-agent hooks when it starts, unless the config names it |
| `password-field`, `password-unknown` | R9 | no typing into a password field, nor where the service cannot rule one out (the app is not on the accessibility bus, or nothing in it has focus) |
| `key-like-text`, `card-like-text` | R10 | the text looks like a key, a token, a key's name or a card number |
| `target-not-focused`, `focus-moved` | R11 | typing goes only to the focused window, re-checked before every key event |
| `remote-not-allowed` | R12 | a remote client and a window off the remote allow-list |
| `no-permission` | R14 | no permission covers this client, action and window |

`stop` and `segfault` work for every caller, with or without a config: anyone may stop the tool. The stop ends any
action in progress within 200 ms and holds until the user resumes it.
