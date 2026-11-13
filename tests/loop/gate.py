# SPDX-License-Identifier: GPL-3.0-or-later
"""M4 gate: the agent loop (riley-agent-loop), driven by a made-up OpenAI-compatible server, on the M2 test app.

Run through tests/run-loop.sh. Output: one "PASS <check>" or "FAIL <check>: <actual>, expected <expected>" line per
check, then "PASS" or "FAIL (n checks failed)"; exit 0 only on PASS. With --plant <fault> the faulty variant is built
into the -fault build directory and the run must FAIL on the check that targets the fault.

The desktop part reuses tests/desktop/gate.py (the M2 procedure, imported, not copied): build outside the lock; then,
under ~/.local/bin/dp2-lock and a 5-minute cap, notify, open the test app and the decoy on an empty, unused workspace
on DP-2, check placement and visibility, run the cases, end every process by its PID, switch DP-2 back and check
Marc's windows, the focus and the cursor are as before (also on failure, SIGINT, SIGTERM and the cap). Its safety
rules hold here unchanged. On top of them: the loop runs with QT_QPA_PLATFORM=offscreen and without WAYLAND_DISPLAY,
DISPLAY or HYPRLAND_INSTANCE_SIGNATURE, so it shows nothing and can reach the desktop only through the gate's private
riley-agentd; its permissions name only the two test windows; before each loop run the gate focuses the test app
and checks every point the script will click is inside it, on top and not covered (else nothing more runs).

Interface this gate expects (the builder makes the loop follow it)
-------------------------------------------------------------------
riley-agent-loop --headless --config FILE --socket PATH --task TEXT
    Runs one task without a window, then exits. FILE is the config the service also reads (validated the same way);
    the loop uses the endpoint its own-loop client names. PATH is riley-agentd's socket: every action goes there
    (the service knows the loop as own-loop by its executable, build/src/loop/riley-agent-loop).
    Model requests: POST <base-url>/chat/completions, JSON, not streamed, "model" the endpoint's model, "messages",
    and "tools": function tools named at least windows, see, tree, click, type, with arguments window (the window's
    Hyprland address, 0x...), x and y (integers, logical pixels inside the window: click), text (type) and delay_ms
    (optional, type: ms between key events, as riley-agent's --delay-ms).
    Key: header "Authorization: Bearer <key>" when the endpoint has key-env (that variable's value) or key-file
    (the file's content without leading and trailing whitespace); no Authorization header when it has neither. The
    key is never printed, logged, written anywhere or sent anywhere but in that header.
    The task goes in a user message of the first request. For each tool call of a reply, in order: one request to
    the service, then one message {"role": "tool", "tool_call_id": <the call's id>, "content": C}; C (a string, or
    content parts whose text parts are joined) starts with the service's first line as riley-agent prints it
    ("ok ...", "refused <reason> ...", "stopped ...", "failed ..."), followed by the listing lines for windows and
    tree, one per line.
    Pictures: only for a see the service answered ok, the PNG exactly as the service gave it, as a data URL
    "data:image/png;base64,..." in an image_url content part, either in that tool message's content or in one user
    message right after that reply's tool messages. Never anywhere else, never for a see that was not ok.
    A reply with no tool calls ends the task: last stdout line "done <the reply's text>", exit 0.
    Stop: the task "segfault" (the safe word) or a line "segfault" or "stop" on standard input (the loop's control
    path; the window's task field and stop button take the same path) makes the loop send segfault (or stop) to the
    service, which ends any action in progress; the loop sends no further model request (none at all for the safe
    word as the task), its last stdout line is "stopped ...", exit 3. Any other failure: "failed <what>", exit 4.
    Standard output: one line per step; never the key, never picture data.
riley-agent-window (spec change, lead decision: the window-resume design, which implements R2=A "the tool's window
    may resume")
    The Qt Quick window is its own process, riley-agent-window (comm "riley-agent-win"); for each task it starts
    riley-agent-loop (comm "riley-agent-loo", client own-loop) as its child, passing the task and relaying Stop and
    the safe word over the child's standard input. The service takes a caller with no claude, pi, codex or
    riley-agent-loo among its ancestors' comms as the user, so the window's resume is allowed and the loop's is
    refused (R0 unchanged). The only way the window resumes is its Re-enable button; the model cannot press it (the
    tool's own window is deny-always, R6b). The headless loop above stays as it is. --list-ui moves to the window:
    riley-agent-window --list-ui, offscreen, prints one line per item with an objectName ("<objectName> <QML type>")
    and exits 0 without contacting the service or a model; the items include taskField (the task entry),
    transcriptView, stopButton and reenableButton. The rest of the window's interface (--help, riley::createWindow in
    the library riley_window, the test-only harness tests/loop/harness) and check (f): tests/loop/window_gate.py.
The made-up server (tests/loop/mock_server.py) plays tests/loop/scripts/<case>.json; tool call ids are
"call-<case>-<request n>-<index>", so call-a-2-0 is the first tool call in the reply to request 2 of case a.

Expected results, worked out by hand from the brief, docs/files.md and docs/agents.md before any loop code existed
-------------------------------------------------------------------------------------------------------------------
Config per case (write_loop_config): one client, own-loop, naming one endpoint (base-url the mock's, model
mock-model); remote-allow: the test app only (gate-remote-testapp); permissions loop-app (own-loop: windows see tree
click type on org.riley.testapp "Riley test app" DP-2) and loop-decoy (own-loop: windows see on org.riley.testdecoy
"Riley test decoy" DP-2). The gate's own kind (claude-code or pi) is not listed. Rule L (M1): own-loop is remote if
its location or its endpoint is remote; with no location it takes the endpoint's.
Points (tests/app/main.cpp): button 90,120; plain field 170,176; secret field 170,226 (window-local, logical).
Texts: TEXT_A "riley loop 2026" (15 chars), TEXT_PW "fake words for the gate" (23, made up), TEXT_SLOW the M2 48-char
text. None has a key name, token prefix, 32-char run or 13+ digit run (R10 passes).

 case/request          expected (log line from client own-loop)   deciding rule and why
 --------------------  ------------------------------------------ -------------------------------------------------
 (a) endpoint mock-local, location local, key-file. Task: TASK_A.
 a1 windows            allow allowed per-window done; the listing local (L). Each window decided alone: test app R13
                       is exactly the two test windows             loop-app, decoy R13 loop-decoy; Marc's windows
                                                                   R6/R14 (no permission names them): left out
 a2 see app            allow allowed loop-app done                 R13 (not remote: R12 does not apply)
                       -> request 3 carries exactly one picture, in the see turn: 600x450 (480x360 x 1.25), block
                          rgb(18,164,92) at (75,62), background rgb(240,240,240) at (175,62) (M2's values)
 a3,a4 click button    allow allowed loop-app done (x2)            R13; two tool calls in one reply
 a5 click plain        allow allowed loop-app done                 R13
 a6 type TEXT_A        allow allowed loop-app done, length 15      R9 no (role "text"), R10 no, R11 focused, R13
   -> the app counts exactly 2 more clicks; the plain field holds exactly TEXT_A; exactly 6 model requests (one
      per scripted reply); the tool results reach the model (request 2: windows; 4: both clicks; 5, 6: "ok ...");
      the bar shows acting / own-loop / type during a6; exit 0, last line "done ..."; every request has
      Authorization "Bearer " + the key file's key.
 (b) endpoint mock-remote, location remote, no key. Task: TASK_B.
 b1 windows            allow allowed per-window done; the listing remote (L: endpoint remote). Decoy: R12
                       is the test app alone                       remote-not-allowed, left out; app R13 loop-app
 b2 see app            allow allowed loop-app done                 R12 passes (gate-remote-testapp), R13
 b3 see decoy          deny remote-not-allowed R12 refused         R12 comes before R13: loop-decoy does not help
   -> remote-image: every picture in every request is the test app's (size, block, background as a2) and none
      holds the decoy's colour rgb(201,38,170) (+-12); request 3 holds exactly one picture, in the see-app turn;
      request 4 holds at most that one, and none after the see-decoy call. No request names org.riley.testdecoy or
      "Riley test decoy". Request 4 has the tool result of call-b-3-0 starting "refused remote-not-allowed".
      Exactly 4 requests; no Authorization header; exit 0.
 (d) endpoint mock-local, location local, key-env RILEY_GATE_FAKE_KEY. Task: TASK_D.
 d1 click secret       allow allowed loop-app done                 R13
 d2 type TEXT_PW       deny password-field R9 refused, length 23   R9: the focused control is "password text"
 d3 click button       allow allowed loop-app done                 R13: the loop carries on
   -> request 3 has the tool result of call-d-2-0 starting "refused password-field"; the secret field stays empty
      (length 0); the app counts 1 more click; request 4 has call-d-3-0's result starting "ok"; exactly 4 requests;
      exit 0; every request has Authorization "Bearer " + the variable's key.
 (c1) endpoint mock-local, key-file. Task: "segfault".
 probe (gate's kind)   deny unknown-client R4 (logged as the gate) R4: the gate's kind is not a configured client
 the loop              no request line; control own-loop segfault  the safe word as a task: stop, ask no model
                       done
 probe again           deny stopped R3                             R3 comes before R4
   -> 0 model requests; exit 3, last line "stopped ..."; the service still runs; bar stopped.
 (c2) a fresh private service (fresh state: the stop of c1 is not in it). Endpoint mock-local, key-file.
 c2-1 click plain      allow allowed loop-app done                 R13
 c2-2 type TEXT_SLOW,  allow allowed loop-app stopped, length 48   R13 at the start; ended by segfault, sent as a
      delay_ms 100                                                 line on the loop's standard input after >= 5 keys
                       control own-loop segfault done
   -> the last key reaches the app at most 200 ms after the gate wrote "segfault"; fewer than 48 keys; exit 3 within
      5 s, last line "stopped ..."; exactly 2 model requests, still 2 one second after the loop ended (the third
      reply is never asked for); bar stopped; probe refused stopped.
 (e) after all cases: neither made-up key appears in any file the run wrote (loop output, service output, logs,
     state files, mock records, configs; the key file itself excepted) nor on this gate's output; no picture file
     (PNG or JPEG signature) under the gate's folders or the loop's home.
 (f) window resume, outside the lock, without the desktop: tests/loop/window_gate.py in a bwrap sandbox like the M3
     gate's (the gate's own Claude Code ancestry hidden by a private process namespace), with its own private
     riley-agentd, stopped first. The harness presses Re-enable: from a plain parent the service resumes (control
     resume done, bar idle; R0 passes: the user); under a parent with comm claude it is refused resume-needs-user
     (R0) and the bar stays stopped; riley-agent resume under a parent with comm riley-agent-loo is refused
     resume-needs-user (R0); riley-agent-window --help and every option it names, and every --name string in its
     binary, never resume. The table with the reasoning per step is in that file.
 UI (in the same sandbox): riley-agent-window --list-ui, offscreen, lists taskField, transcriptView, stopButton and
     reenableButton; exit 0 within 15 s; no service contact.
Log files: one per case (log-<case>.xml), each line one element; own-loop request lines exactly as the table, in
order, with class/title/monitor of the target (empty for windows), text-length and text-hmac (HMAC-SHA-256 with the
hash secret) for typed requests and no typed text, plain SHA-256 or text-sha256 anywhere.

Live smoke: after the run, outside the lock, the gate asks 127.0.0.1 ports 8000 8080 8081 1234 11434 30000 for
/v1/models (2 s each). It never runs a real model itself: it prints SKIP when none answers, else INFO naming the
port (a smoke run against it is done by hand, with the lead's word). Not a check.
"""
import argparse
import base64
import hashlib
import hmac
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess
import sys
import time
import urllib.request
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True                  # no __pycache__ in the repo from the import below
_spec = importlib.util.spec_from_file_location('riley_desktop_gate', REPO / 'tests/desktop/gate.py')
desktop = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(desktop)

SCRATCH = Path('/srv/devops/scratch/riley-desktop-agent/loop-gate')
desktop.SCRATCH = SCRATCH                       # the M2 code writes its logs here for this gate
BUILD, BUILD_FAULT = desktop.BUILD, desktop.BUILD_FAULT
KNOWN_FAULTS = {'loop-image-leak': 'remote-image'}   # fault -> the check (name prefix) that must catch it
SCRIPTS = REPO / 'tests/loop/scripts'
MOCK = REPO / 'tests/loop/mock_server.py'

OUTPUT = desktop.OUTPUT
APP_CLASS, APP_TITLE = desktop.APP_CLASS, desktop.APP_TITLE
DECOY_CLASS, DECOY_TITLE = desktop.DECOY_CLASS, desktop.DECOY_TITLE
BUTTON, PLAIN, SECRET = desktop.BUTTON, desktop.PLAIN, desktop.SECRET
DECOY_RGB, DECOY_TOLERANCE = (201, 38, 170), 12    # tests/app/main.cpp: the decoy's colour block
STOP_LIMIT_MS = desktop.STOP_LIMIT_MS

TEXT_A = 'riley loop 2026'
TEXT_PW = 'fake words for the gate'                 # made up; refused before any key event
TEXT_SLOW = desktop.TEXT_SLOW                       # 48 characters
SLOW_DELAY_MS = 100
TASK_A = 'Click Count click twice in the Riley test app, then type riley loop 2026 into Plain field.'
TASK_B = 'Look at the Riley test app and at the other test window.'
TASK_D = 'Type some words into Secret field of the Riley test app, then click Count click.'
TASK_C2 = 'Type the numbers one to ten into Plain field of the Riley test app.'
SAFE_WORD = 'segfault'

# Made-up keys, labelled fake. Neither is a real key of any service; they only show where a key goes.
KEY_FILE_VALUE = 'fake-test-key-not-real-file-0001'
KEY_ENV_VALUE = 'fake-test-key-not-real-env-0002'
KEY_ENV_NAME = 'RILEY_GATE_FAKE_KEY'

LOOP_TOOLS = {'windows', 'see', 'tree', 'click', 'type'}
WINDOW_GATE = REPO / 'tests/loop/window_gate.py'
EXIT_DONE, EXIT_STOPPED = 0, 3
RUN_TIMEOUT_S = 40
SMOKE_PORTS = (8000, 8080, 8081, 1234, 11434, 30000)
LOOP_NOTICE = 'Desktop agent test: the agent loop clicks and types in a test window on DP-2 for about a minute'


class LoopGate(desktop.Gate):
    def __init__(self, plant, build):
        super().__init__(plant, build)
        self.scripts = {}

    def set_binaries(self):
        super().set_binaries()
        self.loop = self.build / 'src/loop/riley-agent-loop'
        self.window = self.build / 'src/loop/riley-agent-window'
        self.harness = self.build / 'tests/loop/harness/riley-window-harness'

    # --- outside the lock ---------------------------------------------------------------------------------------
    def build_phase(self, check_placement=False):
        if not self.check_scripts():
            return False
        cmake = ['cmake', '-S', str(REPO), '-B', str(self.build), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug',
                 f'-DRILEY_PLANTED_FAULT={self.plant}']
        with open(SCRATCH / 'configure.log', 'w') as log:
            ok = subprocess.run(cmake, stdout=log, stderr=log).returncode == 0
        if not self.check('build: cmake configure', ok, f'failed (see {SCRATCH}/configure.log)', 'configured'):
            return False
        built = True
        for t in ('riley-test-app', 'riley-agentd', 'riley-agent', 'riley-agent-loop'):
            with open(SCRATCH / f'build-{t}.log', 'w') as log:
                ok = subprocess.run(['cmake', '--build', str(self.build), '--target', t, '-j8'], stdout=log,
                                    stderr=log).returncode == 0
            built &= self.check(f'build: {t}{" with planted fault " + self.plant if self.plant else ""}', ok,
                                f'did not build (see {SCRATCH}/build-{t}.log)', 'built')
        # The window and its test harness (check f): built on their own, so (a)-(e) still run when they do not build.
        for t in ('riley-agent-window', 'riley-window-harness'):
            with open(SCRATCH / f'build-{t}.log', 'w') as log:
                ok = subprocess.run(['cmake', '--build', str(self.build), '--target', t, '-j8'], stdout=log,
                                    stderr=log).returncode == 0
            self.check(f'window-resume: build: {t}{" with planted fault " + self.plant if self.plant else ""}', ok,
                       f'did not build (see {SCRATCH}/build-{t}.log)', 'built')
        if not built:
            return False
        self.set_binaries()
        for b in (self.app_bin, self.agentd, self.cli, self.loop):
            if not self.check(f'build: {b.name} at {b.relative_to(self.build)}', os.access(b, os.X_OK), 'missing',
                              'an executable'):
                return False
        kind = desktop.gate_client_kind()
        return self.check('client: this gate runs under a coding agent', kind is not None,
                          'neither claude nor pi among its ancestors', 'claude (Claude Code) or pi (Pi)')

    def check_scripts(self, report=True):
        """The scripts say what this gate's table says (texts, points, delay), so the two cannot drift apart."""
        want = {'a': ['windows', 'see', 'click', 'click', 'click', 'type'], 'b': ['windows', 'see', 'see'],
                'c1': [], 'c2': ['click', 'type'], 'd': ['click', 'type', 'click']}
        files = {'a': 'a-task.json', 'b': 'b-remote.json', 'c1': 'c1-safe-word.json', 'c2': 'c2-segfault-midway.json',
                 'd': 'd-denied.json'}
        problems = []
        for case, name in files.items():
            try:
                script = json.loads((SCRIPTS / name).read_text())
            except (OSError, ValueError) as error:
                problems.append(f'{name}: {error}')
                continue
            self.scripts[case] = script
            calls = [c for r in script.get('replies', []) for c in r.get('tool_calls', [])]
            if script.get('case') != case or [c['name'] for c in calls] != want[case]:
                problems.append(f'{name}: case {script.get("case")!r}, calls {[c["name"] for c in calls]}')
            for c in calls:
                a = c.get('arguments', {})
                if c['name'] == 'click' and (a.get('x'), a.get('y')) not in (BUTTON, PLAIN, SECRET):
                    problems.append(f'{name}: click at {a.get("x")},{a.get("y")} is not a known control')
                if c['name'] in ('see', 'click', 'type', 'tree') and a.get('window') not in ('${APP}', '${DECOY}'):
                    problems.append(f'{name}: {c["name"]} on {a.get("window")!r}')
        texts = {case: [c['arguments'].get('text') for r in s.get('replies', []) for c in r.get('tool_calls', [])
                        if c['name'] == 'type'] for case, s in self.scripts.items()}
        if texts.get('a') != [TEXT_A] or texts.get('d') != [TEXT_PW] or texts.get('c2') != [TEXT_SLOW]:
            problems.append(f'typed texts {texts}')
        c2 = [c for r in self.scripts.get('c2', {}).get('replies', []) for c in r.get('tool_calls', [])
              if c['name'] == 'type']
        if not c2 or c2[0]['arguments'].get('delay_ms') != SLOW_DELAY_MS:
            problems.append('c2: type is not slowed to 100 ms between keys')
        if not report:
            return not problems
        return self.check('scripts: tests/loop/scripts match this gate\'s table', not problems, '; '.join(problems),
                          'the calls, points and texts of the table')

    # --- under the lock ----------------------------------------------------------------------------------------
    def desktop_checks(self):
        subprocess.run(['notify-send', LOOP_NOTICE], timeout=5, capture_output=True)
        keys = self.run_dir / 'keys'
        keys.mkdir(mode=0o700)
        self.key_file = keys / 'endpoint-key'
        self.key_file.write_text(KEY_FILE_VALUE + '\n')
        os.chmod(self.key_file, 0o600)
        for case in (self.case_a, self.case_b, self.case_d, self.case_c1, self.case_c2):
            try:
                case()
            except desktop.Unsafe:
                raise
            except Exception as error:      # a bug in the gate or a reply it cannot read: that case fails
                self.check(f'{case.__name__}: ran to the end', False, f'{error.__class__.__name__}: {error}',
                           'no error')
            finally:
                self.end_case()
        self.check_key_nowhere()
        self.check_no_pictures()

    # --- per case -----------------------------------------------------------------------------------------------
    def begin_case(self, case, location, key):
        self.case = case
        self.mock_record = self.run_dir / f'mock-{case}.jsonl'
        port_file = self.run_dir / f'mock-{case}.port'
        vars_file = self.run_dir / f'mock-{case}.vars.json'
        vars_file.write_text(json.dumps({'APP': self.app_addr, 'DECOY': self.decoy_addr}))
        script = self.run_dir / f'script-{case}.json'
        script.write_text(json.dumps(self.scripts[case]))
        env = {k: os.environ[k] for k in ('PATH', 'LANG') if k in os.environ}
        self.mock = self.start([sys.executable, '-I', str(MOCK), '--script', str(script), '--vars', str(vars_file),
                                '--record', str(self.mock_record), '--port-file', str(port_file)], env, self.run_dir,
                               f'mock-{case}.stderr')
        deadline = time.monotonic() + 5
        port = ''
        while time.monotonic() < deadline and not port and self.mock.poll() is None:
            port = port_file.read_text().strip() if port_file.exists() else ''
            time.sleep(0.05)
        if not self.check(f'{case}: the made-up server listens on 127.0.0.1', port.isdigit(), port or 'no port',
                          'a port'):
            raise RuntimeError('the made-up server did not start')
        self.config = self.state_dir / f'config-{case}.xml'
        self.log = self.state_dir / f'log-{case}.xml'
        write_loop_config(self.config, int(port), location, key, self.key_file)
        r = subprocess.run(['xmllint', '--noout', '--schema', str(REPO / 'schema/config.xsd'), str(self.config)],
                           capture_output=True, text=True)
        if not self.check(f'{case}: its config is valid to schema/config.xsd', r.returncode == 0,
                          r.stderr.strip()[:200], 'valid'):
            raise RuntimeError('invalid config')
        if not self.start_service():
            raise RuntimeError('the private service did not start')

    def end_case(self):
        for name in ('loop_proc', 'service', 'mock'):
            p = getattr(self, name, None)
            if p is not None and p.poll() is None:
                p.terminate()          # by its own PID
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait(timeout=3)
            if name == 'loop_proc' and p is not None and p.stdin:
                try:
                    p.stdin.close()
                except OSError:
                    pass
            setattr(self, name, None)

    def ready_for(self, case):
        """The test app has focus and every point the script clicks is safe; else Unsafe (nothing more runs)."""
        self.type_ready()
        for r in self.scripts[case]['replies']:
            for c in r.get('tool_calls', []):
                if c['name'] == 'click':
                    self.safe_point((c['arguments']['x'], c['arguments']['y']))

    def loop_env(self, extra=None):
        env = {k: os.environ[k] for k in ('PATH', 'LANG') if k in os.environ}
        env.update(HOME=str(self.home), XDG_RUNTIME_DIR=os.environ['XDG_RUNTIME_DIR'], QT_QPA_PLATFORM='offscreen',
                   XDG_CONFIG_HOME=str(self.home / '.config'), XDG_STATE_HOME=str(self.home / '.local/state'),
                   XDG_CACHE_HOME=str(self.home / '.cache'))
        env.update(extra or {})
        return env

    def run_loop(self, task, extra_env=None):
        out = open(SCRATCH / f'loop-{self.case}.stdout', 'w')
        err = open(SCRATCH / f'loop-{self.case}.stderr', 'w')
        p = subprocess.Popen([str(self.loop), '--headless', '--config', str(self.config), '--socket',
                              str(self.socket), '--task', task], env=self.loop_env(extra_env), cwd=self.home,
                             stdin=subprocess.PIPE, stdout=out, stderr=err)
        out.close()
        err.close()
        self.procs.append(p)
        self.loop_proc = p
        return p

    def wait_loop(self, p, timeout=RUN_TIMEOUT_S, watch_bar=False):
        seen = set()
        deadline = time.monotonic() + timeout
        while p.poll() is None and time.monotonic() < deadline:
            if watch_bar:
                b = self.bar()
                if b:
                    seen.add((b.get('state'), b.get('client'), b.get('action')))
            time.sleep(0.02)
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait(timeout=3)
            rc = f'no end within {timeout} s'
        else:
            rc = p.returncode
        lines = [l for l in (SCRATCH / f'loop-{self.case}.stdout').read_text(errors='replace').splitlines()
                 if l.strip()]
        return rc, (lines[-1].strip() if lines else ''), seen

    def model_requests(self):
        """The chat-completions requests the made-up server received, in order."""
        out = []
        if self.mock_record.exists():
            for line in self.mock_record.read_text().splitlines():
                try:
                    e = json.loads(line)
                except ValueError:
                    continue
                if e.get('method') == 'POST' and e.get('path') == '/v1/chat/completions':
                    out.append(e)
        return out

    def probe(self):
        """A see from the gate itself (its kind is not configured, so never allowed): shows the service's state."""
        png = self.runtime / 'probe.png'
        kind, line = self.agent('see', '--window', self.app_addr, '--png', str(png))
        if png.exists():
            png.unlink()
        return line

    def check_finished(self, case, rc, last, n, want_n):
        self.check(f'{case}: the loop finishes the task (exit 0, last line "done ...")',
                   rc == EXIT_DONE and last.startswith('done'), f'exit {rc}, last line {last[:100]!r}',
                   'exit 0, done ...')
        self.check(f'{case}: exactly {want_n} model requests, one per scripted reply', n == want_n, n, want_n)

    def check_requests_shape(self, case, reqs, task):
        problems = []
        for e in reqs:
            body = e.get('body') if isinstance(e.get('body'), dict) else {}
            names = {(t.get('function') or {}).get('name') for t in body.get('tools') or [] if isinstance(t, dict)}
            if body.get('model') != 'mock-model':
                problems.append(f'request {e["n"]}: model {body.get("model")!r}')
            if not LOOP_TOOLS <= names:
                problems.append(f'request {e["n"]}: tools {sorted(n for n in names if n)}')
        if reqs:
            first = reqs[0].get('body') or {}
            users = [text_of(m.get('content')) for m in first.get('messages') or []
                     if isinstance(m, dict) and m.get('role') == 'user']
            if not any(task in u for u in users):
                problems.append('the first request has no user message with the task')
        self.check(f'{case}: every request names model mock-model and offers the tools {sorted(LOOP_TOOLS)}; the first '
                   f'carries the task', not problems, '; '.join(problems[:4]), 'all of them')

    def check_result(self, case, reqs, n, call, prefix, what):
        body = reqs[n - 1]['body'] if len(reqs) >= n else {}
        got = tool_result(body, call)
        self.check(f'{case}: {what}', got is not None and got.lstrip().startswith(prefix),
                   'no tool result' if got is None else repr(got[:120]), f'request {n}: tool result of {call} '
                   f'starting {prefix!r}')
        return got

    def check_auth(self, case, reqs, key):
        want = None if key is None else hashlib.sha256(f'Bearer {key}'.encode()).hexdigest()
        got = [(e.get('auth') or {}).get('sha256') for e in reqs]
        if key is None:
            self.check(f'key: {case}: no Authorization header (the endpoint names no key)', all(g is None for g in got),
                       f'{sum(g is not None for g in got)} requests with one', 'none')
        else:
            self.check(f'key: {case}: every request has Authorization "Bearer <the made-up key>"',
                       bool(got) and all(g == want for g in got),
                       f'{sum(g == want for g in got)} of {len(got)} requests', 'all of them')

    def check_case_log(self, case, rows, controls):
        try:
            text = self.log.read_text()
            key = self.secret.read_bytes()
        except OSError as error:
            self.check(f'{case}: log: log-{case}.xml written', False, error, 'a file')
            return
        own, ctrl, bad = [], [], 0
        for raw in text.splitlines():
            if not raw.strip():
                continue
            try:
                e = ET.fromstring(raw)
            except ET.ParseError:
                bad += 1
                continue
            if e.get('client') != 'own-loop':
                continue
            if e.tag == 'request':
                own.append(e)
            elif e.tag == 'control':
                ctrl.append((e.get('action'), e.get('result')))
        self.check(f'{case}: log: every line is one well-formed element', bad == 0, f'{bad} bad lines', 0)
        summary = [f'{e.get("action")} {e.get("decision")} {e.get("reason")} {e.get("result")}' for e in own]
        want_summary = [f'{r[0]} {r[1]} {r[2]} {r[4]}' for r in rows]
        self.check(f'{case}: log: the own-loop request lines are exactly the table\'s action sequence',
                   summary == want_summary, summary, want_summary)
        for i, (row, e) in enumerate(zip(rows, own), 1):
            action, decision, reason, rule, result, target, typed = row
            cls, title, mon = {'app': (APP_CLASS, APP_TITLE, OUTPUT), 'decoy': (DECOY_CLASS, DECOY_TITLE, OUTPUT),
                               None: ('', '', '')}[target]
            fields = dict(action=action, decision=decision, reason=reason, rule=rule, result=result,
                          **{'class': cls}, title=title, monitor=mon)
            problems = [f'{k}={e.get(k)!r} (expected {v!r})' for k, v in fields.items() if (e.get(k) or '') != v]
            if typed is None:
                if 'text-length' in e.attrib or 'text-hmac' in e.attrib:
                    problems.append('text fields on a request without text')
            else:
                if e.get('text-length') != str(len(typed)):
                    problems.append(f'text-length={e.get("text-length")!r} (expected {len(typed)})')
                if e.get('text-hmac') != hmac.new(key, typed.encode(), hashlib.sha256).hexdigest():
                    problems.append('text-hmac is not HMAC-SHA-256 of the text with the secret file')
            self.check(f'{case}: log: line {i} ({action} {decision} {reason} {rule} {result})', not problems,
                       '; '.join(problems), 'every field as in the table')
        self.check(f'{case}: log: own-loop control lines', ctrl == controls, ctrl, controls)
        typed = [r[6] for r in rows if r[6]]
        leaked = [t for t in typed if t in text or hashlib.sha256(t.encode()).hexdigest() in text]
        self.check(f'{case}: log: no typed text, no plain SHA-256 of it', not leaked and 'text-sha256' not in text,
                   f'{len(leaked)} found', 'none')

    # --- (a) a scripted task on a local endpoint -----------------------------------------------------------------
    def case_a(self):
        self.begin_case('a', 'local', 'file')
        self.ready_for('a')
        clicks, plain = self.clicks(), self.field('plain')
        p = self.run_loop(TASK_A)
        rc, last, seen = self.wait_loop(p, watch_bar=True)
        time.sleep(0.5)
        reqs = self.model_requests()
        self.check_finished('task', rc, last, len(reqs), 6)
        self.check('task: the app counted exactly 2 clicks', self.clicks() - clicks == 2, self.clicks() - clicks, 2)
        self.check('task: the plain field holds exactly the text sent', plain == '' and self.field('plain') == TEXT_A,
                   f'{plain!r} -> {self.field("plain")!r}', f"'' -> {TEXT_A!r}")
        self.check_requests_shape('task', reqs, TASK_A)
        listing = self.check_result('task', reqs, 2, 'call-a-1-0', 'ok', 'the windows result reaches the model')
        listed = sorted(listed_addresses(listing))
        self.check('task: windows lists exactly the two test windows (local: each window decided by permission)',
                   listed == sorted([self.app_addr, self.decoy_addr]), listed, sorted([self.app_addr, self.decoy_addr]))
        body3 = reqs[2]['body'] if len(reqs) >= 3 else {}
        images = find_images(body3)
        turn = images_in_turn(body3, 'call-a-2-0')
        ok, what = (app_picture(images[0][1]) if len(images) == 1 else (False, f'{len(images)} pictures'))
        self.check('task: request 3 carries exactly one picture, in the see turn, of the test app',
                   len(images) == 1 and len(turn) == 1 and ok, f'{len(images)} pictures, {len(turn)} in the turn: {what}',
                   '1 picture: 600x450, block (18,164,92), background (240,240,240)')
        self.check_result('task', reqs, 4, 'call-a-3-0', 'ok', 'the first click\'s result reaches the model')
        self.check_result('task', reqs, 4, 'call-a-3-1', 'ok', 'the second click\'s result reaches the model')
        self.check_result('task', reqs, 5, 'call-a-4-0', 'ok', 'the field click\'s result reaches the model')
        self.check_result('task', reqs, 6, 'call-a-5-0', 'ok', 'the type result reaches the model')
        self.check('bar: acting, client own-loop, action type, while the loop types', ('acting', 'own-loop', 'type') in seen,
                   sorted(seen, key=str), ('acting', 'own-loop', 'type'))
        self.check_auth('a', reqs, KEY_FILE_VALUE)
        self.check_case_log('a', [
            ('windows', 'allow', 'allowed', 'per-window', 'done', None, None),
            ('see', 'allow', 'allowed', 'loop-app', 'done', 'app', None),
            ('click', 'allow', 'allowed', 'loop-app', 'done', 'app', None),
            ('click', 'allow', 'allowed', 'loop-app', 'done', 'app', None),
            ('click', 'allow', 'allowed', 'loop-app', 'done', 'app', None),
            ('type', 'allow', 'allowed', 'loop-app', 'done', 'app', TEXT_A),
        ], [])

    # --- (b) a remote endpoint: no picture of a window off the remote allow-list ----------------------------------
    def case_b(self):
        self.begin_case('b', 'remote', None)
        self.type_ready()
        p = self.run_loop(TASK_B)
        rc, last, _ = self.wait_loop(p)
        time.sleep(0.3)
        reqs = self.model_requests()
        self.check_finished('remote', rc, last, len(reqs), 4)
        self.check_requests_shape('remote', reqs, TASK_B)
        listing = self.check_result('remote', reqs, 2, 'call-b-1-0', 'ok', 'the windows result reaches the model')
        listed = listed_addresses(listing)
        self.check('remote: windows lists the test app alone (the decoy is off the remote allow-list)',
                   listed == [self.app_addr], listed, [self.app_addr])

        bad = []
        for e in reqs:
            for idx, data in find_images(e.get('body')):
                ok, what = app_picture(data)
                if not ok:
                    bad.append(f'request {e["n"]} message {idx}: {what}')
        self.check('remote-image: every picture in every request is the test app\'s, none shows the decoy\'s colour',
                   not bad, '; '.join(bad[:3]), 'only 600x450 pictures of the test app, no rgb(201,38,170)')
        body3 = reqs[2]['body'] if len(reqs) >= 3 else {}
        n3, turn3 = len(find_images(body3)), len(images_in_turn(body3, 'call-b-2-0'))
        self.check('remote-image: request 3 carries exactly one picture, in the see-app turn', n3 == 1 and turn3 == 1,
                   f'{n3} pictures, {turn3} in the turn', '1, in the turn of call-b-2-0')
        body4 = reqs[3]['body'] if len(reqs) >= 4 else {}
        all4 = find_images(body4)
        app4 = images_in_turn(body4, 'call-b-2-0')
        self.check('remote-image: no picture is sent for the refused see of the decoy (request 4: at most the app\'s, '
                   'none after the see-decoy call)', len(all4) <= 1 and len(app4) == len(all4),
                   f'{len(all4)} pictures, {len(all4) - len(app4)} outside the see-app turn', 'at most 1, all in it')
        self.check_result('remote', reqs, 4, 'call-b-3-0', 'refused remote-not-allowed',
                          'the decoy refusal reaches the model as a tool result naming the reason')
        named = [e['n'] for e in reqs if DECOY_CLASS in json.dumps(e.get('body'))
                 or DECOY_TITLE in json.dumps(e.get('body'), ensure_ascii=False)]
        self.check('remote-text: no request names the decoy\'s class or title', not named, f'requests {named}', 'none')
        self.check_auth('b', reqs, None)
        self.check_case_log('b', [
            ('windows', 'allow', 'allowed', 'per-window', 'done', None, None),
            ('see', 'allow', 'allowed', 'loop-app', 'done', 'app', None),
            ('see', 'deny', 'remote-not-allowed', 'R12', 'refused', 'decoy', None),
        ], [])

    # --- (d) a denied action is refused and reported back ---------------------------------------------------------
    def case_d(self):
        self.begin_case('d', 'local', 'env')
        self.ready_for('d')
        clicks = self.clicks()
        p = self.run_loop(TASK_D, {KEY_ENV_NAME: KEY_ENV_VALUE})
        rc, last, _ = self.wait_loop(p)
        time.sleep(0.5)
        reqs = self.model_requests()
        self.check_finished('denied', rc, last, len(reqs), 4)
        self.check_requests_shape('denied', reqs, TASK_D)
        self.check_result('denied', reqs, 3, 'call-d-2-0', 'refused password-field',
                          'typing into the password field is refused and the next request carries the reason')
        self.check('denied: the password field stays empty', self.field('secret', 'length') == 0,
                   f'length {self.field("secret", "length")}', 'length 0')
        self.check_result('denied', reqs, 4, 'call-d-3-0', 'ok', 'the loop carries on: the next click is done')
        self.check('denied: the app counted the one click after the refusal', self.clicks() - clicks == 1,
                   self.clicks() - clicks, 1)
        self.check_auth('d', reqs, KEY_ENV_VALUE)
        self.check_case_log('d', [
            ('click', 'allow', 'allowed', 'loop-app', 'done', 'app', None),
            ('type', 'deny', 'password-field', 'R9', 'refused', 'app', TEXT_PW),
            ('click', 'allow', 'allowed', 'loop-app', 'done', 'app', None),
        ], [])

    # --- (c) the safe word ---------------------------------------------------------------------------------------
    def case_c1(self):
        self.begin_case('c1', 'local', 'file')
        line = self.probe()
        self.check('safe-word: before the stop, a request from the gate itself is refused unknown-client (R4)',
                   line.split()[:2] == ['refused', 'unknown-client'], line, 'refused unknown-client')
        p = self.run_loop(SAFE_WORD)
        rc, last, _ = self.wait_loop(p, timeout=10)
        time.sleep(0.5)
        reqs = self.model_requests()
        self.check('safe-word: the task "segfault" ends the loop (exit 3, last line "stopped ...")',
                   rc == EXIT_STOPPED and last.startswith('stopped'), f'exit {rc}, last line {last[:100]!r}',
                   'exit 3, stopped ...')
        self.check('safe-word: no model request at all', len(reqs) == 0, len(reqs), 0)
        self.check('safe-word: the service still runs', self.service.poll() is None, f'exit {self.service.poll()}',
                   'running')
        b = self.bar()
        self.check('safe-word: bar stopped', bool(b) and b.get('state') == 'stopped', b, 'state stopped')
        line = self.probe()
        self.check('safe-word: the service is stopped (a request is refused with stopped, R3)',
                   line.split()[:2] == ['refused', 'stopped'], line, 'refused stopped')
        self.check_case_log('c1', [], [('segfault', 'done')])

    def case_c2(self):
        self.begin_case('c2', 'local', 'file')      # a fresh private service: the stop of c1 is not in its state
        self.ready_for('c2')
        since = desktop.now_ms()
        p = self.run_loop(TASK_C2)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and len(self.keys_since('main', since)) < 5 and p.poll() is None:
            time.sleep(0.01)
        started = len(self.keys_since('main', since))
        t0 = desktop.now_ms()
        try:
            p.stdin.write(b'segfault\n')
            p.stdin.flush()
        except OSError:
            pass
        rc, last, _ = self.wait_loop(p, timeout=5)
        ended = desktop.now_ms()
        time.sleep(1)
        keys = self.keys_since('main', since)
        late = keys[-1]['ms'] - t0 if keys else None
        reqs = self.model_requests()
        self.check('segfault-midway: typing was under way when segfault was sent', started >= 5, f'{started} keys',
                   'at least 5')
        self.check(f'segfault-midway: the last key arrived within {STOP_LIMIT_MS} ms of segfault',
                   late is not None and late <= STOP_LIMIT_MS and len(keys) < len(TEXT_SLOW),
                   f'{late} ms, {len(keys)} of {len(TEXT_SLOW)} keys', f'<= {STOP_LIMIT_MS} ms, fewer than all')
        self.check('segfault-midway: the loop ends, saying stopped (exit 3, within 5 s)',
                   rc == EXIT_STOPPED and last.startswith('stopped'),
                   f'exit {rc} after {ended - t0} ms, last line {last[:100]!r}', 'exit 3, stopped ...')
        self.check('segfault-midway: no model request after the stop (exactly 2, still 2 a second later)',
                   len(reqs) == 2, len(reqs), 2)
        b = self.bar()
        self.check('segfault-midway: bar stopped', bool(b) and b.get('state') == 'stopped', b, 'state stopped')
        line = self.probe()
        self.check('segfault-midway: the service is stopped (refused stopped, R3)',
                   line.split()[:2] == ['refused', 'stopped'], line, 'refused stopped')
        self.check_auth('c2', reqs, KEY_FILE_VALUE)
        self.check_case_log('c2', [
            ('click', 'allow', 'allowed', 'loop-app', 'done', 'app', None),
            ('type', 'allow', 'allowed', 'loop-app', 'stopped', 'app', TEXT_SLOW),
        ], [('segfault', 'done')])

    # --- (e) and the pictures, over everything the run wrote --------------------------------------------------------
    def check_key_nowhere(self):
        found = []
        for root in (SCRATCH, self.run_dir):
            for path in root.rglob('*'):
                if path == self.key_file or not path.is_file() or path.is_symlink():
                    continue
                try:
                    data = path.read_bytes()
                except OSError:
                    continue
                for k in (KEY_FILE_VALUE, KEY_ENV_VALUE):
                    if k.encode() in data:
                        found.append(str(path.relative_to(SCRATCH)))
        self.check('key: neither made-up key is in any file the run wrote (output, logs, state, requests, configs)',
                   not found, sorted(set(found))[:5], 'none')

    def check_no_pictures(self):
        found = []
        for root in (SCRATCH, self.run_dir, self.home):
            for path in root.rglob('*'):
                try:
                    if path.is_file() and not path.is_symlink():
                        with open(path, 'rb') as f:
                            head = f.read(8)
                        if head.startswith(b'\x89PNG') or head.startswith(b'\xff\xd8\xff'):
                            found.append(str(path))
                except OSError:
                    pass
        self.check('pictures: no picture file under the gate\'s folders or the loop\'s home', not found, found[:5],
                   'none')


# --- helpers -------------------------------------------------------------------------------------------------------

def write_loop_config(path, port, location, key, key_file):
    name = f'mock-{location}'
    key_xml = {None: '', 'file': f'\n      <key-file>{key_file}</key-file>\n    ',
               'env': f'\n      <key-env>{KEY_ENV_NAME}</key-env>\n    '}[key]
    path.write_text(f'''<?xml version="1.0" encoding="UTF-8"?>
<!-- Written by tests/loop/gate.py for one case of the loop gate. The endpoint is the gate's made-up server. -->
<riley-desktop-agent version="1">
  <endpoints>
    <endpoint name="{name}" base-url="http://127.0.0.1:{port}/v1" model="mock-model" location="{location}">{key_xml}</endpoint>
  </endpoints>
  <clients>
    <client kind="own-loop" endpoint="{name}"/>
  </clients>
  <remote-allow>
    <window id="gate-remote-testapp" class="{APP_CLASS}" title="{APP_TITLE}" monitor="{OUTPUT}"/>
  </remote-allow>
  <permissions>
    <allow id="loop-app" client="own-loop" actions="windows see tree click type">
      <window class="{APP_CLASS}" title="{APP_TITLE}" monitor="{OUTPUT}"/>
    </allow>
    <allow id="loop-decoy" client="own-loop" actions="windows see">
      <window class="{DECOY_CLASS}" title="{DECOY_TITLE}" monitor="{OUTPUT}"/>
    </allow>
  </permissions>
</riley-desktop-agent>
''')
    os.chmod(path, 0o600)


def text_of(content):
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return '\n'.join(p.get('text', '') for p in content if isinstance(p, dict) and p.get('type') == 'text')
    return ''


def messages(body):
    m = body.get('messages') if isinstance(body, dict) else None
    return m if isinstance(m, list) else []


def tool_result(body, call_id):
    for m in messages(body):
        if isinstance(m, dict) and m.get('role') == 'tool' and m.get('tool_call_id') == call_id:
            return text_of(m.get('content'))
    return None


def listed_addresses(listing):
    return [l.split()[0] for l in (listing or '').splitlines()[1:] if l.strip().startswith('0x')]


B64 = re.compile(r'[A-Za-z0-9+/=]+')


def pictures_in_string(s):
    """Every picture a string carries: data URLs anywhere in it, or the whole string as bare base64 PNG/JPEG."""
    found = []
    for m in re.finditer(r'base64,', s):
        run = B64.match(s, m.end())
        found.append(run.group(0) if run else '')
    bare = s.strip()
    if not found and (bare.startswith('iVBORw0KGgo') or bare.startswith('/9j/')):
        found.append(bare)
    out = []
    for b in found:
        try:
            out.append(base64.b64decode(b + '=' * (-len(b) % 4)))
        except ValueError:
            out.append(b'')
    return out


def find_images(body):
    """(message index or None, bytes) for every picture anywhere in a request body."""
    out = []

    def walk(v, idx):
        if isinstance(v, str):
            out.extend((idx, d) for d in pictures_in_string(v))
        elif isinstance(v, list):
            for x in v:
                walk(x, idx)
        elif isinstance(v, dict):
            for x in v.values():
                walk(x, idx)
    if isinstance(body, dict):
        for i, m in enumerate(messages(body)):
            walk(m, i)
        walk({k: v for k, v in body.items() if k != 'messages'}, None)
    else:
        walk(body, None)
    return out


def images_in_turn(body, call_id):
    """The pictures after the assistant message that made call_id and before the next assistant message."""
    msgs = messages(body)
    start = None
    for i, m in enumerate(msgs):
        if isinstance(m, dict) and m.get('role') == 'assistant' and \
                any(isinstance(c, dict) and c.get('id') == call_id for c in m.get('tool_calls') or []):
            start = i
    if start is None:
        return []
    end = next((i for i in range(start + 1, len(msgs))
                if isinstance(msgs[i], dict) and msgs[i].get('role') == 'assistant'), len(msgs))
    return [(i, d) for i, d in find_images(body) if i is not None and start < i < end]


def app_picture(data):
    """(ok, what): the test app's picture (M2's size, block and background) with no pixel of the decoy's colour."""
    ok, what = desktop.picture_matches(data)
    from PIL import Image
    try:
        img = Image.open(io.BytesIO(data)).convert('RGB')
        colours = img.getcolors(maxcolors=1 << 24) or []
        decoy = any(all(abs(a - b) <= DECOY_TOLERANCE for a, b in zip(rgb, DECOY_RGB)) for _, rgb in colours)
    except Exception:
        decoy = False
    if decoy:
        return False, what + ', holds the decoy\'s colour'
    return ok, what


def smoke_probe():
    for port in SMOKE_PORTS:
        try:
            with urllib.request.urlopen(f'http://127.0.0.1:{port}/v1/models', timeout=2) as r:
                if r.status == 200 and isinstance(json.loads(r.read(65536) or b'{}').get('data'), list):
                    return port
        except Exception:
            pass
    return None


def run_sandboxed(gate):
    """Check (f) and the UI check: tests/loop/window_gate.py in a bwrap sandbox like the M3 gate's (tests/run-cli.sh).
    Its result lines are passed on and its failures counted."""
    out = SCRATCH / 'window'
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(mode=0o700)
    if shutil.which('bwrap') is None:
        gate.check('window-resume: sandbox: bwrap is installed', False, 'not found', 'bwrap')
        return
    runtime = f'/run/user/{os.getuid()}'
    argv = ['bwrap', '--die-with-parent', '--new-session',
            '--unshare-pid', '--unshare-net', '--unshare-ipc', '--unshare-uts', '--unshare-cgroup-try',
            '--ro-bind', '/', '/', '--dev', '/dev', '--proc', '/proc',
            '--tmpfs', '/tmp', '--tmpfs', '/home', '--perms', '0700', '--tmpfs', runtime,
            '--ro-bind', str(REPO), str(REPO), '--ro-bind', str(gate.build), str(gate.build),
            '--bind', str(out), str(out),
            '--clearenv', '--setenv', 'PATH', '/usr/bin:/bin', '--setenv', 'LANG', 'C.UTF-8',
            '--setenv', 'HOME', '/tmp/riley-window-gate', '--setenv', 'XDG_RUNTIME_DIR', runtime,
            '--setenv', 'HYPRLAND_INSTANCE_SIGNATURE', 'riley-cli-gate-fake-hyprland',
            '--chdir', '/tmp',
            'python3', '-I', str(WINDOW_GATE), '--build', str(gate.build), '--out', str(out)]
    child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True)
    seen_fail = False
    try:
        for line in child.stdout:
            if line.strip() in ('PASS',) or line.startswith('FAIL ('):
                continue                    # the sandbox's own final line: this gate prints the overall one
            print(line, end='', flush=True)
            if line.startswith('FAIL '):
                gate.fails.append(line[5:].strip())
                seen_fail = True
        rc = child.wait(timeout=600)
    except subprocess.TimeoutExpired:
        child.kill()                        # by its own PID; --die-with-parent ends what it started
        rc = child.wait()
    if rc != 0 and not seen_fail:
        gate.check('window-resume: sandbox phase ended with a result', False, f'exit {rc}, no result lines',
                   'its checks')


def run_locked(gate, args):
    """The desktop phase as a child under dp2-lock; its result lines are passed on and its failures counted."""
    argv = [str(desktop.DP2_LOCK), sys.executable, '-I', str(Path(__file__).resolve()), '--desktop-phase',
            '--plant', args.plant]
    child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True)

    def forward(signum, frame):
        kids = Path(f'/proc/{child.pid}/task/{child.pid}/children')
        try:
            pids = [int(x) for x in kids.read_text().split()]
        except OSError:
            pids = []
        for pid in pids or [child.pid]:
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass
    signal.signal(signal.SIGTERM, forward)
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    seen_fail = False
    for line in child.stdout:
        print(line, end='', flush=True)
        if line.startswith('FAIL '):
            gate.fails.append(line[5:].strip())
            seen_fail = True
    rc = child.wait()
    if rc != 0 and not seen_fail:
        gate.check('desktop phase: ended with a result', False, f'exit {rc}, no result lines', 'its checks')


def main():
    parser = argparse.ArgumentParser(description='M4 loop gate (run it through tests/run-loop.sh)')
    parser.add_argument('--plant', default='', help='build this planted fault into the -fault build dir')
    parser.add_argument('--desktop-phase', action='store_true', help=argparse.SUPPRESS)   # internal: under dp2-lock
    args = parser.parse_args()
    if args.plant and args.plant not in KNOWN_FAULTS:
        print(f'unknown planted fault: {args.plant} (known: {", ".join(KNOWN_FAULTS)})')
        return 2
    SCRATCH.mkdir(parents=True, exist_ok=True)
    gate = LoopGate(args.plant, BUILD_FAULT if args.plant else BUILD)
    if args.desktop_phase:
        if not gate.check_scripts(report=False):
            gate.check('scripts: tests/loop/scripts match this gate\'s table', False, 'they changed', 'as checked')
            return 1
        gate.desktop_phase()
        return 1 if gate.fails else 0
    for old in list(SCRATCH.glob('loop-*.std*')) + list(SCRATCH.glob('mock-*.stderr')):
        old.unlink()
    if gate.build_phase():
        run_sandboxed(gate)
        run_locked(gate, args)
    port = smoke_probe()
    if port is None:
        print(f'SKIP live smoke: no OpenAI-compatible endpoint answers /v1/models on 127.0.0.1 ports '
              f'{" ".join(map(str, SMOKE_PORTS))}')
    else:
        print(f'INFO live smoke: an endpoint answers on 127.0.0.1:{port}; the gate runs no real model (by hand, '
              f'with the lead\'s word)')
    if args.plant:
        target = KNOWN_FAULTS[args.plant]
        caught = [f for f in gate.fails if f.startswith(target + ':')]
        print(f'PLANTED FAULT {args.plant} ' + (f'CAUGHT by: {", ".join(caught)}' if caught
                                                 else f'NOT CAUGHT (no failed "{target}" check)'))
    if not gate.fails:
        print('PASS')
        return 0
    print(f'FAIL ({len(gate.fails)} checks failed)')
    return 1


if __name__ == '__main__':
    sys.exit(main())
