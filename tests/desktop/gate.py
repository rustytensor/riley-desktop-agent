# SPDX-License-Identifier: GPL-3.0-or-later
"""M2 gate: the service (riley-agentd) and its actions, on DP-2, against the test app this gate starts.

Run through tests/run-desktop.sh. Output: one "PASS <check>" or "FAIL <check>: <actual>, expected <expected>" line per
check, then "PASS" or "FAIL (n checks failed)"; exit 0 only on PASS. With --plant <fault> the faulty variant is built
into the -fault build directory and the run must FAIL on the check that targets the fault.

Interface this gate expects (the builder makes the programs follow it)
----------------------------------------------------------------------
riley-agentd --config FILE --socket PATH --state-file PATH --log PATH --secret-file PATH
    Runs in the foreground until SIGTERM. Listens on PATH (a Unix socket; the gate makes its folder 0700). Reads
    and validates FILE. Writes the bar state file (docs/files.md, state.json) to --state-file, the request log
    (docs/files.md, log.xml) to --log, and keeps the log's hash key in --secret-file: 32 random bytes, created 0600
    when missing. These options only say where files are; none of them weakens a check.
riley-agent --socket PATH <subcommand> ...
    click --window ADDR X Y                 left click at X,Y: logical pixels inside the window
    type  --window ADDR [--delay-ms N] TEXT  type TEXT into the window's focused control, one key event at a time,
                                             N ms between key events (default the service's own)
    see   --window ADDR --png FILE           a screenshot of the window, written to FILE (the gate gives a path in
                                             its 0700 folder under $XDG_RUNTIME_DIR and deletes it after reading)
    tree  --window ADDR                      the window's accessibility tree: one line per node, "<role>: <name>",
                                             indented by depth
    stop | segfault | resume
    ADDR is the window's Hyprland address as `hyprctl clients -j` prints it (0x...).
    The first line on stdout is the result; its first word is the result kind:
        ok ...                         allowed and done                     exit 0
        refused <reason> ...           refused; <reason> is a reason code   exit not 0
        stopped ...                    ended by stop or segfault midway     exit not 0
Log lines (docs/files.md, log.xml) for a request that carries text have `text-length` and `text-hmac` (lower-case hex
HMAC-SHA-256 of the UTF-8 text, keyed with the 32 bytes of the secret file) and no `text-sha256`. This replaces the
plain SHA-256 of M1 (open item a).

Expected results, worked out by hand from the brief and docs/files.md before any service code existed
-------------------------------------------------------------------------------------------------------
The gate's config (write_config) lists one client, the kind this gate runs under (Claude Code or Pi, found from the
gate's own process ancestry), as remote; the test app (class org.riley.testapp, title "Riley test app", DP-2) is on
the remote allow-list, and permission "gate-act" allows see, tree, click and type on it. The decoy window
(org.riley.testdecoy) is on no list and has no permission.

 #   request                                   expected                       deciding rule and why
 --  ----------------------------------------  -----------------------------  ---------------------------------------
 1-3 click "Count click" (x3)                  allow, rule gate-act, done     R13: client listed (R4), action known
                                                                              (R5), no built-in or config deny (R6-R8),
                                                                              on the remote allow-list (R12)
     -> the app counts 3 clicks (3 sent)
 4   click the text field                      allow gate-act done            R13, as above
 5   type "hello riley 2026"                   allow gate-act done            R13: not a password field (R9); no key
                                                                              name, no token prefix, no 32-char run,
                                                                              no 13+ digit run (R10); focused (R11)
     -> the text field holds exactly "hello riley 2026"
 6   click the password field                  allow gate-act done            R13
 7   type "not a real password" (fake)         deny password-field R9 refused R9: focused control is "password text"
     -> the password field stays empty (length 0)
 8   click the text field                      allow gate-act done            R13
 9   type "focus test words go here", 150 ms   deny focus-moved R11 refused   R11: at the re-check before a key event
     between keys; after >= 3 keys arrive the                                 the active window is the decoy, not the
     gate focuses the decoy                                                   target
     -> the keys the app got are a prefix of the text, at least 3 and fewer than all 24; the decoy gets at most 1 key
        (one key event may already be on its way when the gate switches focus: the re-check comes before each key,
        and the gate switches right after a key arrived, 150 ms before the next is due)
 10  see                                       allow gate-act done            R13 (R12 passes: on the allow-list)
     -> a PNG of 600x450: the window's 480x360 logical size times DP-2's scale 1.25; at pixel (75,62), the centre of
        the colour block (logical 20,20 80x60 -> physical 25,25 100x75), rgb(18,164,92); at pixel (175,62) (logical
        140,50, background) rgb(240,240,240); each channel within 3
 11  tree                                      allow gate-act done            R13
     -> lines "button: Count click", "text: Plain field", "password text: Secret field", "list: Fruit list"
        (the AT-SPI role names Qt 6.11 and libatspi 2.62 give these controls, see TREE_EXPECTED)
 12  click the text field                      allow gate-act done            R13
 13  type 48 chars, 100 ms between keys;       allow gate-act stopped         R13 at the start; ended by stop
     riley-agent stop after >= 5 keys arrive
     -> the last key the app gets comes at most 200 ms after the gate ran stop; fewer than 48 keys arrive; the
        command says "stopped"; while it types the bar state is acting, with this client and action type; after
        stop it is stopped
 14  click "Count click" while stopped         deny stopped R3 refused        R3: stopped
 15  type "after stop" while stopped           deny stopped R3 refused        R3
 16  see while stopped                         deny stopped R3 refused        R3; no PNG written
     -> the click count does not change
     resume                                    ok; bar idle (or watching)
 17  click "Count click"                       allow gate-act done            R13: not stopped any more
     -> the click count goes up by 1 (to 4)
 18  click the text field                      allow gate-act done            R13
 19  type 48 chars, 100 ms; riley-agent        allow gate-act stopped         as 13
     segfault after >= 5 keys
 20  click "Count click" while stopped         deny stopped R3 refused        R3
     resume
 21  click "Count click"                       allow gate-act done            R13 (count 5)
Bar state file: idle before the first request (docs/files.md: idle = show nothing); acting/client/type during 13
and 19; stopped after stop and after segfault; idle or watching after resume. One line of JSON, version 1, 0600.
Log: one line per request above (21 lines with an action), in order, each with this client, the action, class
org.riley.testapp, title "Riley test app", monitor DP-2 and the decision, reason, rule and result above; the typed
requests (5, 7, 9, 13, 15, 19) with text-length = the text's length and text-hmac as above; no request's text, plain
SHA-256 or `text-sha256` attribute anywhere in the file. Log 0600, secret file 0600 and 32 bytes.

Safety (a live session, Marc's desktop)
---------------------------------------
- Acts only on the two windows it starts (by their own process IDs and Hyprland addresses). Every Hyprland dispatch
  names one of those addresses, except putting the cursor back (hl.dsp.cursor.move). No reload, no keyword, no
  plugin, no config file is touched.
- Refuses to start (FAIL, no click, no cursor move) if DP-2 is missing or not as expected, if DP-2 shows a special
  workspace or a fullscreen or maximized window, if the focused monitor's workspace has one (a new window opens
  there first and could take it out of fullscreen), or if the test app is not where it was placed.
- Before every click: the point must be inside the test app's rectangle, the test app must be the active window,
  and no other floating, pinned or fullscreen window may cover the point. If not, that check fails and nothing more
  is clicked or typed in this run.
- One notification before the first cursor move; the cursor is put back where it was at the end.
- Everything it starts is ended by its own process ID. Screenshots stay in memory, or for `see` in a 0700 folder
  under $XDG_RUNTIME_DIR, deleted as soon as read. The test texts are made up; nothing typed is a key or password.
"""
import argparse
import hashlib
import hmac
import io
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timezone

REPO = Path(__file__).resolve().parents[2]
BUILD = Path('/srv/devops/forge/build/riley-desktop-agent')
BUILD_FAULT = Path('/srv/devops/forge/build/riley-desktop-agent-fault')
SCRATCH = Path('/srv/devops/scratch/riley-desktop-agent/desktop-gate')
KNOWN_FAULTS = {'focus-recheck-removed': 'focus-moved'}   # fault -> the check that must catch it

# --- Fixed expected values -----------------------------------------------------------------------------------------
OUTPUT = 'DP-2'
MONITOR = dict(x=1440, y=1847, width=2560, height=1600, scale=1.25)   # the lead's desktop facts, 2026-11-12
APP_CLASS, APP_TITLE = 'org.riley.testapp', 'Riley test app'
DECOY_CLASS, DECOY_TITLE = 'org.riley.testdecoy', 'Riley test decoy'
APP_SIZE, DECOY_SIZE = (480, 360), (240, 120)                       # tests/app/main.cpp, setFixedSize
APP_AT = (MONITOR['x'] + 200, MONITOR['y'] + 200)                   # 1640,2047: clear of the bar and the dock
DECOY_AT = (MONITOR['x'] + 760, MONITOR['y'] + 200)                 # 2200,2047: beside the app, not over it
# Window-local logical points (tests/app/main.cpp layout): the centre of each control.
BUTTON = (90, 120)
PLAIN = (170, 176)
SECRET = (170, 226)
PNG_SIZE = (round(APP_SIZE[0] * MONITOR['scale']), round(APP_SIZE[1] * MONITOR['scale']))   # 600x450
BLOCK_RGB, BLOCK_PX = (18, 164, 92), (75, 62)       # logical (60,50) x 1.25 = (75,62.5)
BACKGROUND_RGB, BACKGROUND_PX = (240, 240, 240), (175, 62)   # logical (140,50) x 1.25
COLOUR_TOLERANCE = 3
STOP_LIMIT_MS = 200                                 # the brief: stop ends an action within 200 ms
# AT-SPI role names of the four controls (QPushButton, QLineEdit, QLineEdit in Password mode, QListWidget).
# "password text" is the lead's desktop fact. All four were read with python3-gobject's Atspi (libatspi 2.62) from
# the test app built with Qt 6.11.2, on a private Xvfb display, on 2026-11-12, before any service code existed: Qt's
# own mapping, not the code under test. libatspi 2.62 names ATSPI_ROLE_PUSH_BUTTON "button".
TREE_EXPECTED = [('button', 'Count click'), ('text', 'Plain field'), ('password text', 'Secret field'),
                 ('list', 'Fruit list')]

TEXT_PLAIN = 'hello riley 2026'
TEXT_SECRET = 'not a real password'                 # fake; refused before any key event
TEXT_FOCUS = 'focus test words go here'             # 24 characters
TEXT_SLOW = 'one two three four five six seven eight nine ten'   # 48 characters
TEXT_STOPPED = 'after stop'
FOCUS_DELAY_MS, SLOW_DELAY_MS = 150, 100
ACTIONS = {'see', 'tree', 'windows', 'point', 'click', 'type', 'key', 'launch', 'close'}
NOTICE = 'Riley desktop agent test: clicking and typing in a test window on DP-2 for about a minute'


class Unsafe(Exception):
    """A live-session rule would be broken: nothing more is clicked, typed or moved in this run."""


class Gate:
    def __init__(self, plant, build):
        self.plant = plant
        self.build = build
        self.fails = []
        self.procs = []          # every process this gate started, ended by its own PID at the end
        self.owned = set()       # Hyprland addresses of the windows this gate started
        self.unsafe = None
        self.cursor = None
        self.requests = []       # expected log lines, in order
        self.started_at = datetime.now(timezone.utc)

    # --- output ---------------------------------------------------------------------------------------------------
    def check(self, name, ok, actual='', expected=''):
        if ok:
            print(f'PASS {name}', flush=True)
        else:
            detail = f': {actual}, expected {expected}' if (actual != '' or expected != '') else ''
            print(f'FAIL {name}{detail}', flush=True)
            self.fails.append(name)
        return ok

    # --- Hyprland ---------------------------------------------------------------------------------------------------
    def hypr(self, what):
        if what not in ('monitors', 'clients', 'activewindow', 'cursorpos', 'workspaces'):
            raise Unsafe(f'hyprctl {what} is not a query this gate makes')
        out = subprocess.run(['hyprctl', what, '-j'], capture_output=True, text=True, timeout=3, check=True).stdout
        return json.loads(out) if out.strip() else {}

    def dispatch(self, expr, address=None):
        if address is not None and address not in self.owned:
            raise Unsafe(f'dispatch against a window this gate did not start: {address}')
        if address is None and not expr.startswith('hl.dsp.cursor.move('):
            raise Unsafe(f'dispatch without a window address: {expr}')
        if address is not None and f'"address:{address}"' not in expr:
            raise Unsafe(f'dispatch does not name its window: {expr}')
        r = subprocess.run(['hyprctl', 'dispatch', expr], capture_output=True, text=True, timeout=3)
        if r.returncode != 0 or 'error' in (r.stdout + r.stderr).lower():
            raise Unsafe(f'hyprctl dispatch {expr!r} failed: {(r.stdout + r.stderr).strip()[:200]}')

    def monitor(self):
        for m in self.hypr('monitors'):
            if m.get('name') == OUTPUT:
                return m
        return None

    def client(self, address):
        for c in self.hypr('clients'):
            if c.get('address') == address:
                return c
        return None

    def focus(self, address):
        self.dispatch(f'hl.dsp.focus({{ window = "address:{address}" }})', address)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if self.hypr('activewindow').get('address') == address:
                return True
            time.sleep(0.05)
        return False

    # --- processes --------------------------------------------------------------------------------------------------
    def start(self, argv, env, cwd, log_name):
        err = open(SCRATCH / log_name, 'w')
        p = subprocess.Popen(argv, env=env, cwd=cwd, stdin=subprocess.DEVNULL, stdout=err, stderr=err)
        err.close()
        self.procs.append(p)
        return p

    def end_all(self):
        alive = []
        for p in reversed(self.procs):
            if p.poll() is None:
                p.terminate()        # by its own PID
                try:
                    p.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait(timeout=3)
            if p.poll() is None:
                alive.append(p.pid)
        return alive

    # --- the command line -------------------------------------------------------------------------------------------
    def agent(self, *args, timeout=20):
        try:
            r = subprocess.run([str(self.cli), '--socket', str(self.socket), *args], env=self.env, cwd=self.home,
                               capture_output=True, text=True, timeout=timeout, stdin=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            return 'timeout', f'no answer within {timeout} s'
        line = (r.stdout.splitlines() or [''])[0].strip()
        kind = line.split(' ', 1)[0] if line else f'(no output, exit {r.returncode}, stderr {r.stderr.strip()[:120]!r})'
        if (kind == 'ok') != (r.returncode == 0):
            kind = f'{kind} with exit {r.returncode}'
        return kind, line

    def expect_request(self, action, decision, reason, rule, result, text=None):
        self.requests.append(dict(action=action, decision=decision, reason=reason, rule=rule, result=result,
                                  text=text))

    # --- the test app -------------------------------------------------------------------------------------------------
    def events(self, name):
        path = self.report / f'{name}.jsonl'
        if not path.exists():
            return []
        out = []
        for line in path.read_text().splitlines():
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
        return out

    def keys_since(self, name, since_ms, widget=None):
        return [e for e in self.events(name) if e.get('event') == 'key' and e['ms'] >= since_ms
                and (widget is None or e.get('widget') == widget)]

    def clicks(self):
        counts = [e['count'] for e in self.events('main') if e.get('event') == 'click']
        return counts[-1] if counts else 0

    def field(self, widget, key='value'):
        values = [e.get(key) for e in self.events('main') if e.get('event') == 'field' and e.get('widget') == widget]
        return values[-1] if values else ('' if key == 'value' else 0)

    def bar(self):
        try:
            return json.loads(self.state_file.read_text())
        except (OSError, ValueError):
            return None

    # --- safety -------------------------------------------------------------------------------------------------------
    def safe_point(self, local):
        """Raises Unsafe unless the window-local point is on the test app, which is active and not covered."""
        c = self.client(self.app_addr)
        if c is None:
            raise Unsafe('the test app window is gone')
        (x, y), (w, h) = c['at'], c['size']
        if (x, y) != APP_AT or (w, h) != APP_SIZE:
            raise Unsafe(f'the test app moved: at {x},{y} size {w}x{h}, placed at {APP_AT} size {APP_SIZE}')
        gx, gy = x + local[0], y + local[1]
        if not (x + 1 <= gx <= x + w - 2 and y + 1 <= gy <= y + h - 2):
            raise Unsafe(f'point {gx},{gy} is not inside the test app')
        if self.hypr('activewindow').get('address') != self.app_addr:
            raise Unsafe('the test app is not the active window')
        m = self.monitor()
        if m is None or (m.get('specialWorkspace') or {}).get('id', 0) != 0:
            raise Unsafe(f'{OUTPUT} is gone or shows a special workspace')
        ws = c['workspace']['id']
        for o in self.hypr('clients'):
            if o.get('address') == self.app_addr or not o.get('mapped', True) or o.get('hidden'):
                continue
            same = (o.get('workspace') or {}).get('id') == ws or (o.get('pinned') and o.get('monitor') == m['id'])
            if not same:
                continue
            (ox, oy), (ow, oh) = o['at'], o['size']
            covers = ox <= gx < ox + ow and oy <= gy < oy + oh
            if o.get('fullscreen'):
                raise Unsafe(f'a fullscreen window is on the test app\'s workspace ({o.get("class")})')
            if covers and (o.get('floating') or o.get('pinned')):
                raise Unsafe(f'another window covers {gx},{gy} ({o.get("class")})')

    def click(self, local, decision='allow', reason='allowed', rule='gate-act', result='done'):
        self.safe_point(local)
        if self.cursor is None:
            raise Unsafe('no notification was shown before the first cursor move')
        kind, line = self.agent('click', '--window', self.app_addr, str(local[0]), str(local[1]))
        self.expect_request('click', decision, reason, rule, result)
        return kind, line

    def type_ready(self):
        if not self.focus(self.app_addr):
            raise Unsafe('the test app did not take focus')

    # --- the run ------------------------------------------------------------------------------------------------------
    def run(self, check_placement=False):
        try:
            self.prepare(check_placement)
            if check_placement:
                return
            self.desktop_checks()
        except Unsafe as error:
            self.unsafe = str(error)
            self.check('run: stopped early', False, str(error), 'a run to the end')
        finally:
            self.finish()

    def prepare(self, check_placement):
        cmake = ['cmake', '-S', str(REPO), '-B', str(self.build), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug',
                 f'-DRILEY_PLANTED_FAULT={self.plant}']
        with open(SCRATCH / 'configure.log', 'w') as log:
            ok = subprocess.run(cmake, stdout=log, stderr=log).returncode == 0
        if not self.check('build: cmake configure', ok, f'failed (see {SCRATCH}/configure.log)', 'configured'):
            raise Unsafe('not built: nothing started')
        targets = ['riley-test-app'] if check_placement else ['riley-test-app', 'riley-agentd', 'riley-agent']
        built = True
        for t in targets:
            with open(SCRATCH / f'build-{t}.log', 'w') as log:
                ok = subprocess.run(['cmake', '--build', str(self.build), '--target', t, '-j8'], stdout=log,
                                    stderr=log).returncode == 0
            built &= self.check(f'build: {t}{" with planted fault " + self.plant if self.plant else ""}', ok,
                                f'did not build (see {SCRATCH}/build-{t}.log)', 'built')
        if not built:
            raise Unsafe('not built: nothing started')
        self.app_bin = self.build / 'tests/app/riley-test-app'
        self.agentd = self.build / 'src/agentd/riley-agentd'
        self.cli = self.build / 'src/cli/riley-agent'
        for b in ([self.app_bin] if check_placement else [self.app_bin, self.agentd, self.cli]):
            if not self.check(f'build: {b.name} at {b.relative_to(self.build)}', os.access(b, os.X_OK), 'missing',
                              'an executable'):
                raise Unsafe('not built: nothing started')

        self.kind = gate_client_kind()
        if not self.check('client: this gate runs under a coding agent', self.kind is not None,
                          'neither claude nor pi among its ancestors', 'claude (Claude Code) or pi (Pi)'):
            raise Unsafe('the service could not identify the gate: nothing started')

        # DP-2 as the lead described it, showing a normal workspace without a fullscreen window.
        m = self.monitor()
        if not self.check(f'{OUTPUT}: connected', m is not None, 'missing', 'connected'):
            raise Unsafe(f'{OUTPUT} is missing: nothing started')
        got = {k: m.get(k) for k in MONITOR}
        if not self.check(f'{OUTPUT}: at 1440,1847, 2560x1600, scale 1.25', got == MONITOR, got, MONITOR):
            raise Unsafe(f'{OUTPUT} is not as expected: nothing started')
        self.workspace = m['activeWorkspace']['id']
        special = (m.get('specialWorkspace') or {}).get('id', 0)
        full = [w for w in self.hypr('workspaces') if w.get('id') == self.workspace and w.get('hasfullscreen')]
        if not self.check(f'{OUTPUT}: no special workspace shown, no fullscreen window on workspace '
                          f'{self.workspace}', special == 0 and not full, f'special {special}, fullscreen {bool(full)}',
                          'neither'):
            raise Unsafe(f'{OUTPUT} is covered: nothing started')
        # A new window opens on the focused monitor before it is moved to DP-2; with a fullscreen or maximized window
        # there, Hyprland may take that window out of fullscreen (misc:on_focus_under_fullscreen), which would change
        # one of Marc's windows. So that workspace must have none either.
        focused = [x for x in self.hypr('monitors') if x.get('focused')]
        fws = focused[0]['activeWorkspace']['id'] if focused else None
        ffull = [w for w in self.hypr('workspaces') if w.get('id') == fws and w.get('hasfullscreen')]
        if not self.check(f'focused monitor: no fullscreen or maximized window on workspace {fws}',
                          bool(focused) and not ffull, f'fullscreen {bool(ffull)}', 'none'):
            raise Unsafe('a new window could take a window out of fullscreen: nothing started')

        # Folders: scratch run folder (0700) for config, log, secret and the app's reports; runtime folder (0700).
        self.run_dir = SCRATCH / 'run'
        if self.run_dir.exists():
            shutil.rmtree(self.run_dir)
        for d in ('', 'home', 'state', 'report'):
            (self.run_dir / d).mkdir(mode=0o700, parents=True, exist_ok=True)
            os.chmod(self.run_dir / d, 0o700)
        self.home, self.state_dir, self.report = self.run_dir / 'home', self.run_dir / 'state', self.run_dir / 'report'
        runtime = Path(os.environ['XDG_RUNTIME_DIR'])
        self.runtime = runtime / f'riley-desktop-agent-gate-{os.getpid()}'
        self.runtime.mkdir(mode=0o700)
        os.chmod(self.runtime, 0o700)
        self.socket = self.runtime / 'agentd.sock'
        self.state_file = self.runtime / 'state.json'
        self.log = self.state_dir / 'log.xml'
        self.secret = self.state_dir / 'hash-secret'
        self.config = self.state_dir / 'config.xml'
        self.env = clean_env(self.home)

        # The two windows: started here, then placed by dispatching against their own addresses only.
        self.app = self.start([str(self.app_bin), '--report', str(self.report / 'main.jsonl')], self.app_env(),
                              self.home, 'app.stderr')
        self.decoy = self.start([str(self.app_bin), '--report', str(self.report / 'decoy.jsonl'), '--decoy'],
                                self.app_env(), self.home, 'decoy.stderr')
        self.app_addr = self.find_window(self.app, APP_CLASS)
        self.decoy_addr = self.find_window(self.decoy, DECOY_CLASS)
        placed = self.place(self.app_addr, APP_AT, APP_SIZE, 'test app')
        placed &= self.place(self.decoy_addr, DECOY_AT, DECOY_SIZE, 'decoy')
        if not placed:
            raise Unsafe('the test windows are not where they were placed: nothing clicked')
        if not self.check('focus: the test app is the active window', self.focus(self.app_addr), 'not active',
                          'active'):
            raise Unsafe('the test app did not take focus: nothing clicked')

        # The gate's own look (in memory): the test app is visible on top, its colour block where the layout says.
        shot = grab(f'{APP_AT[0]},{APP_AT[1]} {APP_SIZE[0]}x{APP_SIZE[1]}')
        ok, what = picture_matches(shot)
        if not self.check('visible: the gate\'s own screenshot shows the test app on top', ok, what,
                          f'{PNG_SIZE[0]}x{PNG_SIZE[1]}, block {BLOCK_RGB}, background {BACKGROUND_RGB}'):
            raise Unsafe('the test app is not visible as placed: nothing clicked')

        # The gate's own AT-SPI read: the controls are on the accessibility bus (needed for tree and password-field).
        found = atspi_controls('riley-test-app')
        if check_placement:
            print(f'INFO atspi roles of the test app: {found}')
        missing = [f'{r}: {n}' for r, n in TREE_EXPECTED if [r, n] not in found]
        self.check('accessibility: the test app\'s four controls are on the AT-SPI bus', not missing,
                   f'missing {missing}, found {found}', 'all four')

    def app_env(self):
        env = clean_env(self.home)
        env.update(QT_QPA_PLATFORM='wayland', QT_LINUX_ACCESSIBILITY_ALWAYS_ON='1',
                   QT_WAYLAND_DISABLE_WINDOWDECORATION='1')
        return env

    def find_window(self, proc, cls):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise Unsafe(f'{cls} exited ({proc.returncode}) before its window appeared')
            for c in self.hypr('clients'):
                if c.get('pid') == proc.pid and c.get('class') == cls and c.get('mapped', True):
                    self.owned.add(c['address'])
                    return c['address']
            time.sleep(0.1)
        raise Unsafe(f'no {cls} window within 8 s')

    def place(self, address, at, size, what):
        a = f'"address:{address}"'
        self.dispatch(f'hl.dsp.window.move({{ workspace = {self.workspace}, follow = false, window = {a} }})', address)
        self.dispatch(f'hl.dsp.window.float({{ action = "enable", window = {a} }})', address)
        self.dispatch(f'hl.dsp.window.resize({{ x = {size[0]}, y = {size[1]}, window = {a} }})', address)
        self.dispatch(f'hl.dsp.window.move({{ x = {at[0]}, y = {at[1]}, window = {a} }})', address)
        m = self.monitor()
        want = dict(at=list(at), size=list(size), floating=True, workspace=self.workspace, monitor=m['id'],
                    fullscreen=0)
        deadline = time.monotonic() + 3
        while True:
            c = self.client(address) or {}
            got = dict(at=c.get('at'), size=c.get('size'), floating=c.get('floating'),
                       workspace=(c.get('workspace') or {}).get('id'), monitor=c.get('monitor'),
                       fullscreen=c.get('fullscreen'))
            if got == want or time.monotonic() > deadline:
                break
            time.sleep(0.1)
        return self.check(f'placement: {what} floating on {OUTPUT} at {at[0]},{at[1]} size {size[0]}x{size[1]}',
                          got == want, got, want)

    def desktop_checks(self):
        write_config(self.config, self.kind)
        r = subprocess.run(['xmllint', '--noout', '--schema', str(REPO / 'schema/config.xsd'), str(self.config)],
                           capture_output=True, text=True)
        if not self.check('config: the gate\'s test config is valid to schema/config.xsd', r.returncode == 0,
                          r.stderr.strip()[:200], 'valid'):
            return

        env = dict(self.env)
        self.service = self.start([str(self.agentd), '--config', str(self.config), '--socket', str(self.socket),
                                   '--state-file', str(self.state_file), '--log', str(self.log),
                                   '--secret-file', str(self.secret)], env, self.home, 'riley-agentd.stderr')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not self.socket.exists() and self.service.poll() is None:
            time.sleep(0.05)
        if not self.check('service: riley-agentd listening on the gate\'s private socket',
                          self.socket.exists() and self.service.poll() is None,
                          f'socket {self.socket.exists()}, exit {self.service.poll()}', 'listening'):
            return
        time.sleep(0.3)
        b = self.bar()
        self.check('bar: idle before any request', bool(b) and b.get('state') == 'idle' and not b.get('client'),
                   b, 'state idle, no client')

        # From here the service moves the cursor: one notice first, and the position to put it back to.
        pos = self.hypr('cursorpos')
        subprocess.run(['notify-send', NOTICE], timeout=5, capture_output=True)
        self.cursor = (pos['x'], pos['y'])
        time.sleep(2)

        self.check_clicks()
        self.check_type()
        self.check_password()
        self.check_focus_moved()
        self.check_see()
        self.check_tree()
        self.check_stop('stop', first=True)
        self.check_stop('segfault', first=False)
        self.check_log()

    def check_clicks(self):
        before = self.clicks()
        kinds = [self.click(BUTTON)[0] for _ in range(3)]
        time.sleep(0.5)
        self.check('clicks: each of 3 clicks answered ok', kinds == ['ok'] * 3, kinds, ['ok'] * 3)
        self.check('clicks: the app counted 3 clicks for 3 sent', self.clicks() - before == 3,
                   self.clicks() - before, 3)

    def check_type(self):
        self.click(PLAIN)
        self.type_ready()
        kind, line = self.agent('type', '--window', self.app_addr, TEXT_PLAIN)
        self.expect_request('type', 'allow', 'allowed', 'gate-act', 'done', TEXT_PLAIN)
        time.sleep(0.5)
        self.check('type: answered ok', kind == 'ok', line, 'ok')
        self.check('type: the text field holds the text sent', self.field('plain') == TEXT_PLAIN,
                   repr(self.field('plain')), repr(TEXT_PLAIN))

    def check_password(self):
        self.click(SECRET)
        self.type_ready()
        kind, line = self.agent('type', '--window', self.app_addr, TEXT_SECRET)
        self.expect_request('type', 'deny', 'password-field', 'R9', 'refused', TEXT_SECRET)
        time.sleep(0.5)
        reason = line.split()[1] if kind == 'refused' and len(line.split()) > 1 else None
        self.check('password-field: typing refused with reason password-field', reason == 'password-field', line,
                   'refused password-field')
        self.check('password-field: the password field stays empty', self.field('secret', 'length') == 0,
                   f'length {self.field("secret", "length")}', 'length 0')

    def check_focus_moved(self):
        self.click(PLAIN)
        self.type_ready()
        since = now_ms()
        typing = subprocess.Popen([str(self.cli), '--socket', str(self.socket), 'type', '--window', self.app_addr,
                                   '--delay-ms', str(FOCUS_DELAY_MS), TEXT_FOCUS], env=self.env, cwd=self.home,
                                  stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.procs.append(typing)
        self.expect_request('type', 'deny', 'focus-moved', 'R11', 'refused', TEXT_FOCUS)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and len(self.keys_since('main', since)) < 3 and typing.poll() is None:
            time.sleep(0.01)
        got_before = len(self.keys_since('main', since))
        moved = got_before >= 3 and self.focus(self.decoy_addr)
        try:
            out, _ = typing.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            typing.terminate()
            out, _ = typing.communicate(timeout=3)
        time.sleep(0.5)
        line = (out.splitlines() or [''])[0].strip()
        app_keys = ''.join(e.get('text', '') for e in self.keys_since('main', since))
        decoy_keys = self.keys_since('decoy', since)
        problems = []
        if not moved:
            problems.append(f'the gate could not move focus ({got_before} keys arrived first)')
        if line.split()[:2] != ['refused', 'focus-moved']:
            problems.append(f'answer {line!r}')
        if not (3 <= len(app_keys) < len(TEXT_FOCUS) and TEXT_FOCUS.startswith(app_keys)):
            problems.append(f'app got {app_keys!r}')
        if len(decoy_keys) > 1:
            problems.append(f'decoy got {len(decoy_keys)} keys')
        self.check('focus-moved: typing stops with reason focus-moved when focus moves before a key event',
                   not problems, '; '.join(problems),
                   f'refused focus-moved, a prefix of 3..{len(TEXT_FOCUS) - 1} keys in the app, at most 1 in the decoy')
        if not self.focus(self.app_addr):
            raise Unsafe('the test app did not take focus back')

    def check_see(self):
        png = self.runtime / 'see.png'
        kind, line = self.agent('see', '--window', self.app_addr, '--png', str(png))
        self.expect_request('see', 'allow', 'allowed', 'gate-act', 'done')
        data = b''
        if png.exists():
            data = png.read_bytes()
            png.unlink()
        self.check('see: answered ok', kind == 'ok', line, 'ok')
        ok, what = picture_matches(data)
        self.check(f'see: the test app\'s pixels ({PNG_SIZE[0]}x{PNG_SIZE[1]}, block {BLOCK_RGB} at {BLOCK_PX}, '
                   f'background {BACKGROUND_RGB} at {BACKGROUND_PX})', ok, what, 'all three')

    def check_tree(self):
        try:
            r = subprocess.run([str(self.cli), '--socket', str(self.socket), 'tree', '--window', self.app_addr],
                               env=self.env, cwd=self.home, capture_output=True, text=True, timeout=20,
                               stdin=subprocess.DEVNULL)
            out = r.stdout.splitlines()
        except subprocess.TimeoutExpired:
            out = ['timeout']
        self.expect_request('tree', 'allow', 'allowed', 'gate-act', 'done')
        self.check('tree: answered ok', bool(out) and out[0].split(' ', 1)[0] == 'ok', out[:1], 'ok')
        lines = {l.strip() for l in out[1:]}
        for role, name in TREE_EXPECTED:
            self.check(f'tree: finds {role} "{name}"', f'{role}: {name}' in lines, 'not listed', f'"{role}: {name}"')

    def check_stop(self, word, first):
        self.click(PLAIN)
        self.type_ready()
        since = now_ms()
        typing = subprocess.Popen([str(self.cli), '--socket', str(self.socket), 'type', '--window', self.app_addr,
                                   '--delay-ms', str(SLOW_DELAY_MS), TEXT_SLOW], env=self.env, cwd=self.home,
                                  stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.procs.append(typing)
        self.expect_request('type', 'allow', 'allowed', 'gate-act', 'stopped', TEXT_SLOW)
        seen = []
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline and len(self.keys_since('main', since)) < 5 and typing.poll() is None:
            b = self.bar()
            if b:
                seen.append((b.get('state'), b.get('client'), b.get('action')))
            time.sleep(0.02)
        if first:
            self.check('bar: acting, with this client and action type, while typing',
                       ('acting', self.kind, 'type') in seen, sorted(set(seen), key=str),
                       ('acting', self.kind, 'type'))
        started = len(self.keys_since('main', since))
        t0 = now_ms()
        kind, line = self.agent(word)
        try:
            out, _ = typing.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            typing.terminate()
            out, _ = typing.communicate(timeout=3)
        time.sleep(0.5)
        keys = self.keys_since('main', since)
        last = keys[-1]['ms'] if keys else None
        tline = (out.splitlines() or [''])[0].strip()
        self.check(f'{word}: riley-agent {word} answered ok', kind == 'ok', line, 'ok')
        self.check(f'{word}: typing was under way when {word} ran', started >= 5, f'{started} keys', 'at least 5')
        self.check(f'{word}: the typing command says stopped', tline.split(' ', 1)[0] == 'stopped', tline, 'stopped ...')
        late = (last - t0) if last is not None else None
        self.check(f'{word}: the last key arrived within {STOP_LIMIT_MS} ms of {word}',
                   late is not None and late <= STOP_LIMIT_MS and len(keys) < len(TEXT_SLOW),
                   f'{late} ms, {len(keys)} of {len(TEXT_SLOW)} keys', f'<= {STOP_LIMIT_MS} ms, fewer than all')
        b = self.bar()
        self.check(f'bar: stopped after {word}', bool(b) and b.get('state') == 'stopped', b, 'state stopped')

        count = self.clicks()
        kind, line = self.click(BUTTON, 'deny', 'stopped', 'R3', 'refused')
        time.sleep(0.3)
        self.check(f'{word}: a click while stopped is refused with reason stopped',
                   line.split()[:2] == ['refused', 'stopped'] and self.clicks() == count,
                   f'{line!r}, clicks {count} -> {self.clicks()}', 'refused stopped, count unchanged')
        if first:
            self.type_ready()
            kind, line = self.agent('type', '--window', self.app_addr, TEXT_STOPPED)
            self.expect_request('type', 'deny', 'stopped', 'R3', 'refused', TEXT_STOPPED)
            self.check('stop: typing while stopped is refused with reason stopped',
                       line.split()[:2] == ['refused', 'stopped'], line, 'refused stopped')
            png = self.runtime / 'see-stopped.png'
            kind, line = self.agent('see', '--window', self.app_addr, '--png', str(png))
            self.expect_request('see', 'deny', 'stopped', 'R3', 'refused')
            wrote = png.exists()
            if wrote:
                png.unlink()
            self.check('stop: see while stopped is refused with reason stopped, no picture written',
                       line.split()[:2] == ['refused', 'stopped'] and not wrote, f'{line!r}, picture {wrote}',
                       'refused stopped, no picture')

        kind, line = self.agent('resume')
        time.sleep(0.3)
        b = self.bar()
        self.check(f'{word}: riley-agent resume answered ok', kind == 'ok', line, 'ok')
        self.check(f'bar: idle or watching after resume ({word})',
                   bool(b) and b.get('state') in ('idle', 'watching'), b, 'state idle or watching')
        count = self.clicks()
        kind, line = self.click(BUTTON)
        time.sleep(0.3)
        self.check(f'{word}: after resume a click is allowed and counted', kind == 'ok' and self.clicks() == count + 1,
                   f'{line!r}, clicks {count} -> {self.clicks()}', f'ok, count {count + 1}')

    def check_log(self):
        try:
            text = self.log.read_text()
        except OSError as error:
            self.check('log: log.xml written', False, error, 'a file')
            return
        mode = stat.S_IMODE(self.log.stat().st_mode)
        self.check('log: log.xml is 0600', mode == 0o600, oct(mode), '0o600')
        lines, bad = [], 0
        for raw in text.splitlines():
            if not raw.strip():
                continue
            try:
                e = ET.fromstring(raw)
            except ET.ParseError:
                bad += 1
                continue
            if e.tag == 'request' and e.get('action') in ACTIONS:
                lines.append(e)
        self.check('log: every line is one well-formed element', bad == 0, f'{bad} bad lines', 0)
        self.check(f'log: one line per action request ({len(self.requests)})', len(lines) == len(self.requests),
                   len(lines), len(self.requests))
        try:
            key = self.secret.read_bytes()
            smode = stat.S_IMODE(self.secret.stat().st_mode)
        except OSError:
            key, smode = b'', None
        self.check('log: hash secret file is 0600 and 32 bytes', smode == 0o600 and len(key) == 32,
                   f'mode {oct(smode) if smode is not None else None}, {len(key)} bytes', '0o600, 32 bytes')
        start = self.started_at.timestamp() - 1
        for i, (want, e) in enumerate(zip(self.requests, lines), 1):
            problems = []
            fields = dict(client=self.kind, action=want['action'], **{'class': APP_CLASS}, title=APP_TITLE,
                          monitor=OUTPUT, decision=want['decision'], reason=want['reason'], rule=want['rule'],
                          result=want['result'])
            for k, v in fields.items():
                if e.get(k) != v:
                    problems.append(f'{k}={e.get(k)!r} (expected {v!r})')
            try:
                t = datetime.strptime(e.get('time', ''), '%Y-%m-%dT%H:%M:%S.%fZ').replace(tzinfo=timezone.utc)
                if not (start <= t.timestamp() <= time.time() + 1):
                    problems.append(f'time {e.get("time")} outside this run')
            except ValueError:
                problems.append(f'time {e.get("time")!r} not UTC ISO 8601 with ms')
            if 'text-sha256' in e.attrib:
                problems.append('has text-sha256')
            if want['text'] is None:
                if 'text-length' in e.attrib or 'text-hmac' in e.attrib:
                    problems.append('text fields on a request without text')
            else:
                if e.get('text-length') != str(len(want['text'])):
                    problems.append(f'text-length={e.get("text-length")!r} (expected {len(want["text"])})')
                mac = hmac.new(key, want['text'].encode(), hashlib.sha256).hexdigest() if key else None
                if e.get('text-hmac') != mac:
                    problems.append('text-hmac is not HMAC-SHA-256 of the text with the secret file')
            self.check(f'log: line {i} ({want["action"]} {want["decision"]} {want["reason"]} {want["result"]})',
                       not problems, '; '.join(problems), 'every field as in the table')
        leaked = [t for t in {r['text'] for r in self.requests if r['text']}
                  if t in text or hashlib.sha256(t.encode()).hexdigest() in text]
        self.check('log: no typed text and no plain SHA-256 of it in log.xml', not leaked and 'text-sha256' not in text,
                   f'{len(leaked)} texts found', 'none')

    def finish(self):
        alive = self.end_all()
        if self.cursor is not None:
            try:
                x, y = self.cursor
                self.dispatch(f'hl.dsp.cursor.move({{x={x}, y={y}}})')
                time.sleep(0.2)
                pos = self.hypr('cursorpos')
                self.check('cursor: put back where it was', abs(pos['x'] - x) <= 1 and abs(pos['y'] - y) <= 1,
                           f'{pos["x"]},{pos["y"]}', f'{x},{y}')
            except (Unsafe, subprocess.SubprocessError, OSError, ValueError) as error:
                self.check('cursor: put back where it was', False, error, f'{self.cursor}')
        self.check('cleanup: every process this gate started has ended', not alive, alive, [])
        runtime = getattr(self, 'runtime', None)
        if runtime is not None and runtime.exists():
            shutil.rmtree(runtime, ignore_errors=True)


# --- helpers -------------------------------------------------------------------------------------------------------

def now_ms():
    return int(time.time() * 1000)


def gate_client_kind():
    """The coding agent this gate runs under, from its own process ancestry (comm of an ancestor)."""
    pid = os.getpid()
    while pid > 1:
        try:
            comm = Path(f'/proc/{pid}/comm').read_text().strip()
            fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        except OSError:
            return None
        if comm == 'claude':
            return 'claude-code'
        if comm == 'pi':
            return 'pi'
        pid = int(fields[1])
    return None


def clean_env(home):
    keep = ('PATH', 'XDG_RUNTIME_DIR', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'HYPRLAND_INSTANCE_SIGNATURE',
            'XDG_SESSION_TYPE', 'LANG')
    env = {k: os.environ[k] for k in keep if k in os.environ}
    env.update(HOME=str(home), XDG_CONFIG_HOME=str(home / '.config'), XDG_STATE_HOME=str(home / '.local/state'),
               XDG_CACHE_HOME=str(home / '.cache'))
    return env


def write_config(path, kind):
    path.write_text(f'''<?xml version="1.0" encoding="UTF-8"?>
<!-- Written by tests/desktop/gate.py for one run of the desktop gate. Its requests come from {kind}. -->
<riley-desktop-agent version="1">
  <clients>
    <client kind="{kind}" location="remote"/>
  </clients>
  <remote-allow>
    <window id="gate-remote-testapp" class="{APP_CLASS}" title="{APP_TITLE}" monitor="{OUTPUT}"/>
  </remote-allow>
  <permissions>
    <allow id="gate-act" client="{kind}" actions="see tree click type">
      <window class="{APP_CLASS}" title="{APP_TITLE}" monitor="{OUTPUT}"/>
    </allow>
  </permissions>
</riley-desktop-agent>
''')
    os.chmod(path, 0o600)


def grab(geometry):
    r = subprocess.run(['grim', '-g', geometry, '-'], capture_output=True, timeout=5)
    return r.stdout if r.returncode == 0 else b''


def picture_matches(data):
    """(ok, what was found) for a PNG of the test app: size, colour block, background."""
    from PIL import Image
    try:
        img = Image.open(io.BytesIO(data)).convert('RGB')
    except Exception as error:   # not a PNG at all
        return False, f'no picture ({error.__class__.__name__})'
    size = img.size
    block, back = img.getpixel(BLOCK_PX) if size == PNG_SIZE else None, \
        img.getpixel(BACKGROUND_PX) if size == PNG_SIZE else None
    near = lambda a, b: a is not None and all(abs(x - y) <= COLOUR_TOLERANCE for x, y in zip(a, b))
    ok = size == PNG_SIZE and near(block, BLOCK_RGB) and near(back, BACKGROUND_RGB)
    return ok, f'{size[0]}x{size[1]}, block {block}, background {back}'


ATSPI_WALK = r'''
import json, sys, gi
gi.require_version("Atspi", "2.0")
from gi.repository import Atspi
desk = Atspi.get_desktop(0)
found = []
def walk(o, d=0):
    if o is None or d > 30 or len(found) > 500: return
    found.append([o.get_role_name(), o.get_name() or ""])
    for i in range(o.get_child_count()):
        try: walk(o.get_child_at_index(i), d + 1)
        except Exception: pass
for i in range(desk.get_child_count()):
    a = desk.get_child_at_index(i)
    if a is not None and (a.get_name() or "") == sys.argv[1]:
        walk(a)
print(json.dumps(found))
'''


def atspi_controls(app_name):
    deadline = time.monotonic() + 6
    found = []
    while time.monotonic() < deadline:
        try:
            r = subprocess.run([sys.executable, '-I', '-c', ATSPI_WALK, app_name], capture_output=True, text=True,
                               timeout=10)
            found = json.loads(r.stdout or '[]')
        except (subprocess.TimeoutExpired, ValueError):
            found = []
        if all([role, name] in found for role, name in TREE_EXPECTED):
            break
        time.sleep(0.5)
    return found


def main():
    parser = argparse.ArgumentParser(description='M2 desktop gate (run it through tests/run-desktop.sh)')
    parser.add_argument('--plant', default='', help='build this planted fault into the -fault build dir')
    parser.add_argument('--check-placement', action='store_true',
                        help='only build and place the test windows, read their AT-SPI roles, then clean up '
                             '(no service, no click, no cursor move)')
    args = parser.parse_args()
    if args.plant and args.plant not in KNOWN_FAULTS:
        print(f'unknown planted fault: {args.plant} (known: {", ".join(KNOWN_FAULTS)})')
        return 2
    SCRATCH.mkdir(parents=True, exist_ok=True)
    gate = Gate(args.plant, BUILD_FAULT if args.plant else BUILD)
    gate.run(check_placement=args.check_placement)
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
