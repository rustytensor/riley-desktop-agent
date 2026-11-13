# Files

What riley-desktop-agent reads and writes, and their formats.

| File | Who writes it | What it holds |
|---|---|---|
| `~/.config/riley-desktop-agent/config.xml` | you | endpoints, clients, permissions, the remote allow-list, the deny-always list |
| `~/.local/state/riley-desktop-agent/log.xml` | the service | one line per request, allowed or refused |
| `~/.local/state/riley-desktop-agent/hash-secret` | the service | the log's hash key: 32 random bytes, 0600 |
| `$XDG_RUNTIME_DIR/riley-desktop-agent/state.json` | the service | what the bar shows |
| `$XDG_RUNTIME_DIR/riley-desktop-agent/agentd.sock` | the service | the socket `riley-agent` talks to |

`riley-agentd --config FILE --socket PATH --state-file PATH --log PATH --secret-file PATH` puts any of these
elsewhere (the tests do); the options only say where files are and change no check.

## config.xml

The schema is `schema/config.xsd`; a copy is compiled into every binary and every load is validated against it.
A missing file, a file that is not well-formed XML, or a file the schema rejects gives no permission at all: every
request is refused (`no-config` or `invalid-config`). `examples/config.xml` is a commented example.

```xml
<riley-desktop-agent version="1">
  <endpoints>
    <endpoint name="local" base-url="http://127.0.0.1:8080/v1" model="some-model" location="local"/>
    <endpoint name="hosted" base-url="https://example.org/v1" model="some-model" location="remote">
      <key-env>SOME_PROVIDER_API_KEY</key-env>      <!-- or <key-file>~/.keys/provider</key-file> -->
    </endpoint>
  </endpoints>
  <clients>
    <client kind="own-loop" endpoint="local"/>
    <client kind="claude-code" location="remote"/>
    <client kind="pi" endpoint="local"/>
  </clients>
  <remote-allow>
    <window id="docs" class="firefox" title="*Qt Documentation*"/>
  </remote-allow>
  <permissions>
    <allow id="pi-look" client="pi" actions="see windows">
      <window class="*" monitor="DP-2"/>
    </allow>
  </permissions>
  <deny-always>
    <window id="family" class="*" title="*Family Photos*"/>
  </deny-always>
  <named-hook-editors>
    <editor class="code"/>
  </named-hook-editors>
</riley-desktop-agent>
```

Sections, in this order (`clients` and `permissions` are required, the others optional):

- **`endpoints`**: OpenAI-compatible endpoints. `name`, `base-url`, `model`, `location` (`local` or `remote`). The
  key is never written in the file: `key-env` names an environment variable, `key-file` names a file (absolute or
  under `~/`) that holds it. An endpoint on this machine needs neither.
- **`clients`**: who may use the tool, at most one of each `kind`: `own-loop` (the tool's own agent loop),
  `claude-code`, `pi`. A client not listed here is refused everything. Each client may name an `endpoint` and a
  `location`. A client is **remote** if its location is `remote`, or its endpoint is remote, or it names neither.
  Claude Code sends what it sees to its provider, so leave it remote unless it runs a local model.
- **`remote-allow`**: the windows a remote client may see or act on. For a remote client, every action (`see`,
  `tree`, `windows`, `point`, `click`, `type`, `key`, `launch`, `close`) is refused on any window not matched here,
  whatever the permissions say: it cannot see those windows, so acting there would be blind.
- **`permissions`**: `allow` entries, each with an `id`, one `client`, a list of `actions` (`see tree windows point
  click type key launch close`) and one or more `window` patterns. The first entry in document order that matches
  the client, the action and the window allows the request; nothing else does.
- **`deny-always`**: windows refused to every client for every action, on top of the built-in list below. Each
  entry needs an `id`; the log names it.
- **`named-hook-editors`**: editors that load a coding agent's hooks (Cursor, VS Code, VSCodium, Windsurf, Zed) are
  refused unless their window class is named here.

**Window patterns** (`window` in all three lists): `class` is required, `title` and `monitor` are optional. Each is a
glob (`*` any run of characters, `?` one character), case-insensitive, matched against the whole value. An absent
title or monitor matches anything. For `launch`, the class is the app being launched.

**Built-in refusals**, which no config can lift: the lock screen; the tool's own window and config; terminals
running a coding agent (Claude Code, Pi, Codex), found from the window's process tree or a terminal title
containing "claude" or "codex"; password managers; banking, payment and crypto apps and sites (by window title words
such as bank, paypal, checkout, payment); system settings and authentication dialogs.

**Typing** (`type`, `key`) is refused into a password field, into a window that does not have focus, and when focus
moved away before the key event. `type` also refuses text that looks like a key or token (variable names such as
`GH_TOKEN` or `..._API_KEY`, known token prefixes, long random-looking strings, 40-character hex strings
included) and card numbers.

### How a request is decided

Rules in order; the first that decides wins. The full table of cases is `tests/policy/cases.xml`.

| Rule | Refused when | Reason |
|---|---|---|
| R1 | the config file is missing | `no-config` |
| R2 | it is not well-formed or not valid | `invalid-config` |
| R3 | the tool is stopped | `stopped` |
| R4 | the client is not configured | `unknown-client` |
| R5 | the action is unknown | `unknown-action` |
| R6 | built-in refusals | `deny-lock-screen`, `deny-own-tool`, `deny-agent-terminal`, `deny-password-manager`, `deny-finance`, `deny-system-settings` |
| R7 | the config's deny-always list | `deny-config-list` |
| R8 | an editor that loads agent hooks, not named | `deny-agent-hook-editor` |
| R9 | typing into a password field | `password-field` |
| R10 | typing key-like text or a card number | `key-like-text`, `card-like-text` |
| R11 | typing into a window without focus, or focus moved | `target-not-focused`, `focus-moved` |
| R12 | a remote client seeing or acting on a window off the remote allow-list | `remote-not-allowed` |
| R13 | allowed by the first matching permission | `allowed` |
| R14 | otherwise | `no-permission` |

## log.xml

One XML element per line, appended; the folder is 0700 and the file 0600. Every request is logged, allowed or
refused. Typed text and key names are never written: only their length and an HMAC-SHA-256 keyed with the
per-install secret in `hash-secret`, so a short text cannot be guessed back from the log by trying likely texts.
Screenshot content is never written.

```xml
<request time="2026-11-12T21:30:05.120Z" client="pi" action="type" class="org.riley.testapp" title="Riley test app" monitor="DP-2" decision="allow" reason="allowed" rule="pi-test-app" result="done" text-length="5" text-hmac="9a0c…"/>
<control time="2026-11-12T21:30:09.004Z" client="claude-code" action="stop" result="done"/>
```

| Attribute | Meaning |
|---|---|
| `time` | UTC, ISO 8601 with milliseconds |
| `client`, `action` | who asked for what |
| `class`, `title`, `monitor` | the target window (empty for `windows`; for `launch` the class is the app) |
| `decision` | `allow` or `deny` |
| `reason` | a reason code from the table above; also `no-window` (rule `service`) when the address names no window |
| `rule` | what decided: a permission `id`, a deny-always `id`, the rule number (`R1` … `R14`), `service`, or `per-window` for `windows` (each window is decided on its own; only the allowed ones are listed) |
| `result` | what happened: `refused`, `done`, `failed`, `stopped` |
| `text-length`, `text-hmac` | only when the request carried text: its length in characters (Unicode code points), and lower-case hex HMAC-SHA-256 of its UTF-8 bytes keyed with the 32 bytes of `hash-secret` |

A typing request that is refused midway (focus moved) is logged once, with the decision that ended it. `stop`,
`segfault` and `resume` are logged as `<control/>` lines (`client` is `unknown` when the caller is no known agent, as
for the stop key).

`hash-secret` is made at the first start (32 bytes from the kernel's random source, mode 0600); the service refuses
to start if it is not exactly 32 bytes of this user with mode 0600. Deleting it starts a new key: older lines can
then no longer be compared.

## state.json

Read by Riley's bar for the pill. Replaced atomically on every change (written to a temporary file, then renamed);
the folder is 0700 and the file 0600. One JSON object on one line:

```json
{"version":1,"state":"acting","client":"claude-code","action":"click","since":"2026-11-12T21:30:05.120Z"}
```

| Field | Values |
|---|---|
| `state` | `idle` (show nothing), `watching` (a client has a session and may see), `acting` (an action is in progress: show the pill red), `stopped` (stopped until resumed) |
| `client` | the client kind, empty when idle |
| `action` | the action in progress, only while acting |
| `since` | when this state began, UTC |

The service writes `idle` when it starts and when it exits, `acting` with the client and the action while one runs,
`watching` with the client between its actions (its session lasts until 10 minutes without a request), and
`stopped` after a stop until `resume`, which goes back to `idle`.

## agentd.sock: the protocol

A Unix stream socket in a folder that must be the user's own with mode 0700 (the service refuses to start
otherwise); the socket is 0600. Only the same user may connect. One request per connection:

1. The caller sends one JSON object on one line, for example
   `{"action":"type","window":"0x55d0c3a2b1f0","text":"hello","delay-ms":50}`.
2. The service answers with one JSON object on one line, then closes:
   `{"kind":"ok","line":"ok typed 5 key events"}`. `kind` is `ok`, `refused`, `stopped` or `failed`; `line` is the
   line `riley-agent` prints first; `body` (an array of lines) is the listing for `windows` and `tree`; `png` (base64)
   is the picture for `see`.

Request fields: `action` (`see tree windows point click type key launch close stop segfault resume`), `window` (the
Hyprland address, `0x…`), `x` and `y` (logical pixels inside the window, for `point` and `click`), `button` (`left`,
`right`, `middle`), `text` (`type`), `delay-ms` (`type`: between key events, 0 to 2000, default 20), `key` (`key`:
`Return`, `ctrl+a`, …), `app` (`launch`: a program name found in `/usr/local/bin:/usr/bin:/bin`).

Who is asking is never taken from the request. The service reads the caller's process ID with `SO_PEERCRED` and
walks its process ancestry in `/proc`: the nearest process named `claude` makes it `claude-code`, `pi` makes it
`pi`, the tool's own loop executable makes it `own-loop`; anything else is no client and is refused (stop and
resume excepted).

How the service fills a request before the engine decides it: the window's class, title and monitor from Hyprland
(`clients`, `monitors`), focus from `activewindow`, the coding agent running in the window from the window's process
and its descendants, and for `type` and `key` whether the focused control is a password field (AT-SPI role
`password text`). One action runs at a time. While typing, right before every key event, the active window is read
again and the engine asked again; if focus moved the typing ends with `refused focus-moved`. `stop` and `segfault`
end the action in progress at once (between key events, before a click, before a picture is sent) and every
request is refused with `stopped` until `resume`. A caller that hangs up ends its own action.

What the actions do: `see` takes the window's rectangle with `grim` into memory and sends it over the socket (the
command line writes it only into a 0700 folder under `$XDG_RUNTIME_DIR`); `tree` walks the window's app on AT-SPI
(at most 30 levels, 2000 nodes); `point` and `click` move the pointer with Hyprland's `hl.dsp.cursor.move` and press
the button on a virtual mouse (`/dev/uinput`, made at start and kept open), only at a point inside the window that
no floating or pinned window covers; `type` and `key` use `wtype`, one key event per call; `launch` starts the
program in a new session in the home folder with a minimal environment (HOME, PATH, the session's display and bus
variables), never in the caller's folder or with its environment; `close` asks Hyprland to close the window. A
notification (`notify-send`) is shown when a client's session starts.
