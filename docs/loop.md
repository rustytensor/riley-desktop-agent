# The agent loop: riley-agent-loop and its window, riley-agent-window

`riley-agent-loop` takes a task, asks a model what to do, acts through `riley-agentd`, sends the results back to the
model, and repeats until the task is done. It has no way to the desktop of its own: every action is one request to
the service, which decides it by `config.xml` and logs it, exactly as for `riley-agent`. The service knows the loop
as the client `own-loop` by its executable.

`riley-agent-window` is the small window to give it a task and watch it. It is its own process: for each task it
starts `riley-agent-loop` as its child and shows the child's output.

## Running it

```
riley-agent-window [--config FILE] [--socket PATH] [--endpoint NAME] [--max-steps N] [--max-seconds N]
riley-agent-window --list-ui
riley-agent-window --help
riley-agent-loop --headless --task TEXT [--config FILE] [--socket PATH] [--endpoint NAME]
                 [--max-steps N] [--max-seconds N]
```

- **The window**: a task field, a Run button, the current state (idle, running, done, stopped, failed), a Stop
  button, a Re-enable button and the transcript of model turns and actions. Each task runs in a new
  `riley-agent-loop --headless` (the one installed next to the window, never one found on `PATH`), started in your
  home folder with the window's options; its output lines are the transcript and its exit status the state. The
  config is read again for each task. Closing the window stops a task that still runs. Any other argument, Qt's own
  options included, is bad usage (exit 2). `--list-ui` builds the window offscreen, prints its named items and exits
  (used by the gate).
- **Headless** (`riley-agent-loop`, which has no window of its own): one task, one line per step on standard
  output, then the last line and the exit status:

  | Last line | Exit | Meaning |
  |---|---|---|
  | `done <the model's answer>` | 0 | the model answered without a tool call, or called `done` |
  | `stopped ...` | 3 | a stop or the safe word ended the task |
  | `failed <what>: ...` | 4 | no config, no endpoint or key, the model failed, or a limit was reached |

  Bad usage exits 2.
- Defaults: `--config` is `~/.config/riley-desktop-agent/config.xml`, `--socket` is
  `$XDG_RUNTIME_DIR/riley-desktop-agent/agentd.sock`, at most 40 model requests (`--max-steps`) and 600 s
  (`--max-seconds`) per task.

## Endpoints

The loop uses the endpoint its `own-loop` client names in `config.xml` (see [files.md](files.md)):

```xml
<endpoints>
  <endpoint name="local" base-url="http://127.0.0.1:8080/v1" model="some-model" location="local"/>
</endpoints>
<clients>
  <client kind="own-loop" endpoint="local"/>
</clients>
```

- Any OpenAI-compatible server works: llama.cpp's server, vLLM, or a hosted one. The loop sends
  `POST <base-url>/chat/completions` with `model`, `messages` and `tools`, not streamed; the model must support tool
  calling, and image input if it is to use `see`.
- **The key** comes from the endpoint's `key-env` (an environment variable's name) or `key-file` (a file's path; its
  content without surrounding whitespace). It is read when a task starts, kept only in memory and sent only in the
  `Authorization: Bearer` header. It is never printed, logged or put in the transcript. An endpoint with neither gets
  no Authorization header. The config never holds a key.
- Redirects are never followed (the key would go with them). A local endpoint is reached directly, never through a
  proxy.
- `--endpoint NAME` picks another endpoint of the config. A **remote** endpoint is refused this way unless the
  service already counts the loop as remote (rule L): otherwise a remote model could get pictures that only a local
  one may see. To use a remote endpoint, name it in the `own-loop` client.

## What the model gets

- The tools `windows`, `see`, `tree`, `point`, `click`, `type`, `key`, `launch`, `close` and `done`, with the same
  meaning as `riley-agent`'s commands. Arguments: `window` (an address from `windows`), `x` and `y` (logical pixels
  inside the window), `button`, `text`, `delay_ms`, `key`, `app`, and `summary` for `done`.
- Every tool call is one request to the service. Its answer goes back as a tool message that starts with the
  service's first line (`ok ...`, `refused REASON (rule R)`, `stopped ...`, `failed ...`), followed by the listing for
  `windows` and `tree`. A refusal reaches the model with its reason; the loop carries on.
- **Pictures** go to the model only for a `see` the service answered `ok`, exactly as the service gave them, in one
  user message after that reply's tool results. A refused `see` sends no picture. Only the newest pictures stay in
  the history. The service applies the remote allow-list, so a remote model never gets a window off it. Pictures are
  never written to disk.

## Stop and the safe word

- The task `segfault` (the safe word), typed in the window or given with `--task`, sends `segfault` to the service
  at once and asks no model. In the window it also ends the task that runs.
- The window's Stop button sends `stop` to the service straight from the window, and `stop` to the running loop on
  its standard input. A line `segfault` or `stop` on the headless loop's standard input does the same from the loop:
  the service revokes every permission and ends the action in progress, the loop drops the model request in flight
  and asks the model nothing more.
- A stop from anywhere else (the stop key, `riley-agent stop`) ends the task too: the next action comes back
  `stopped`.

## Who may end a stop

- **Re-enable** in the window sends `resume` to the service, which decides who may end a stop (rule R0, see
  [files.md](files.md)). The window counts as you when you started it yourself (from your launcher or your own
  terminal), so the service resumes. Started from under Claude Code, Pi or Codex, its resume is refused
  (`resume-needs-user`), and the window says so.
- The loop never resumes: the service refuses `resume` from it, and from anything with a process named
  `riley-agent-loop` among its ancestors.
- The button is the only way the window resumes. No option, standard-input line, socket, D-Bus name or file of the
  window sends `resume`, and no model can press it: the window's class is `riley-desktop-agent`, which the service
  never lets any client see or act on (R6b).
- From a terminal, `riley-agent resume` typed by you does the same.
