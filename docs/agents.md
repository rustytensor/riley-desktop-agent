# Coding agents

How Claude Code and Pi use riley-desktop-agent: through the `riley-agent` command line, called from their Bash
tool. There is no MCP server: a short help text costs far less context than a tool list, and the command line
gives the same decisions as any other way in.

## Setting it up

1. `riley-agentd` runs as a user service (it owns the desktop actions; `riley-agent` only talks to it).
2. `~/.config/riley-desktop-agent/config.xml` lists the agent as a client and gives it permissions
   (`docs/files.md` has the format; `examples/config.xml` a commented example). Everything not allowed there is
   refused.
3. Tell the agent the tool exists, for example in the project's instructions:
   "To see or work the desktop, run `riley-agent help` once and use `riley-agent` through Bash."

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
