#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""M5 gate: the installed build on Marc's desktop. Run it through tests/run-install.sh (its header says how).

Every expected value below was fixed by hand from the plan and the lead's M5 install rules before the installer
existed. They are never changed to make the gate pass; a builder who thinks one is wrong reports a dispute.

THE INSTALLER CONTRACT (tools/install.sh, written by the builder against this gate):
    tools/install.sh --build BUILD --prefix PREFIX --config-dir CONFIG_DIR --unit-dir UNIT_DIR [--start]
  - copies BUILD/src/agentd/riley-agentd, BUILD/src/cli/riley-agent, BUILD/src/loop/riley-agent-loop and
    BUILD/src/loop/riley-agent-window to PREFIX/bin as built (byte for byte: no strip, no RPATH change), mode 0755;
    nothing else goes into PREFIX/bin (no test app, no test harness, no test driver);
  - installs the build's starter config (it comes from the build dir, so a planted fault can reach it) to
    PREFIX/share/riley-desktop-agent/starter-config.xml;
  - CONFIG_DIR/config.xml: written from that starter copy (folder 0700, file 0600) only when no file is there; a file
    that is there is never changed. It prints one line starting "config: wrote " or "config: kept ";
  - writes UNIT_DIR/riley-agentd.service: PartOf= and After= graphical-session.target, ExecStart=PREFIX/bin/riley-agentd,
    Restart=on-failure, WantedBy=graphical-session.target; no sleep inhibitor, no IPC or namespace tricks;
  - --start: systemctl --user daemon-reload, enable riley-agentd, then (re)start it so the new binaries run. Without
    --start it runs no systemctl at all. Exit 0 on success.

MODES
  real     PREFIX ~/.local, CONFIG_DIR ~/.config/riley-desktop-agent, UNIT_DIR ~/.config/systemd/user, --start.
           Checks 1-6.
  staging  --plant FAULT, or --prefix naming a folder under /srv/devops/scratch/riley-desktop-agent/stage.
           CONFIG_DIR PREFIX/config/riley-desktop-agent, UNIT_DIR PREFIX/systemd/user, no --start, a systemctl shim
           first on PATH that records any call. Checks 1-3; 4-6 are skipped (they need the real service). Nothing
           under ~/.local, ~/.config or the user's systemd units may change.

CHECKS
  1  build: cmake configure and build every default target into the build dir (-fault for a planted fault), outside
     any lock.
  2  install: the installer exits 0; each of the 4 binaries at PREFIX/bin is executable and has the sha256 of the
     fresh build's; no test binary there; the starter copy is there and valid to schema/config.xsd; config.xml
     written (absent before: byte-identical to the starter copy, 0600 in a 0700 folder) or kept (there before: same
     sha256 after); the unit file passes `systemd-analyze --user verify` and holds the lines above.
     2k (every mode): a second installer run into a staging prefix whose config.xml already exists (a made-up
     file) keeps it byte for byte and prints "config: kept ".
     Real mode: `systemctl --user is-active riley-agentd` = active, `is-enabled` = enabled, FragmentPath = the unit
     file, its main process runs PREFIX/bin/riley-agentd; $XDG_RUNTIME_DIR/riley-desktop-agent is 0700 and
     agentd.sock in it a socket with mode 0600; state.json there is one line of JSON with version, state, client,
     action, since, and state is "idle" (or "stopped" if the persisted stop copy
     ~/.local/state/riley-desktop-agent/stopped is there).
     Staging mode: the systemctl shim was never called; the user unit's state (systemctl --user show) and the files
     ~/.local/bin/<the 4 binaries>, ~/.config/riley-desktop-agent/config.xml, ~/.config/systemd/user/
     riley-agentd.service are as they were before the run.
  3  the starter config's decision table tests/install/starter-cases.xml (at least 20 cases, hand-worked) through the
     M1 driver tests/policy/tst_policy against PREFIX/share/riley-desktop-agent/starter-config.xml; every reason code
     declared is used and every used one declared; the driver's loader loads it Ok; and its shape as the plan says:
     one endpoint, local, http://127.0.0.1:8080/v1, no key-env or key-file; clients own-loop, claude-code (remote),
     pi (remote) and no other; exactly two allows, each with actions within "see windows"; none for own-loop; no
     named hook editors.
  4  (real mode) the installed service answers `PREFIX/bin/riley-agent windows` run from this session (it must run
     under Claude Code or Pi): exit 0, first line "ok N windows", N lines "ADDR class monitor x,y wxh "title"", every
     listed window on DP-2 by hyprctl clients and by its own line, the terminal this gate runs in not listed (R6c),
     a new log.xml line for it with this session's client and action="windows". Read-only: the cursor and the active
     window are as before.
  5  (real mode) tests/run-policy.sh, run-cli.sh, run-desktop.sh, run-loop.sh, each once with --installed PREFIX;
     each must end with PASS (the desktop ones take dp2-lock themselves and use private service instances).
  6  (real mode) after 5: the installed service is still active with the same main PID, and neither its bar state
     nor the persisted stop copy changed (the earlier gates' private services left it alone).

PLANTED FAULT starter-config-too-wide: built into the -fault build dir, installed only to the staging prefix; the
starter config then also allows pi click on DP-2. Caught by check 3 (case s20, targets="starter-config-too-wide").
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = Path('/srv/devops/forge/build/riley-desktop-agent')
BUILD_FAULT = Path('/srv/devops/forge/build/riley-desktop-agent-fault')
SCRATCH = Path('/srv/devops/scratch/riley-desktop-agent/install-gate')
STAGE = Path('/srv/devops/scratch/riley-desktop-agent/stage')
HOME = Path.home()
REAL_PREFIX = HOME / '.local'
REAL_CONFIG_DIR = HOME / '.config/riley-desktop-agent'
REAL_UNIT_DIR = HOME / '.config/systemd/user'
STOP_COPY = HOME / '.local/state/riley-desktop-agent/stopped'
LOG_XML = HOME / '.local/state/riley-desktop-agent/log.xml'
INSTALLER = REPO / 'tools/install.sh'
SCHEMA = REPO / 'schema/config.xsd'
CASES = REPO / 'tests/install/starter-cases.xml'
KNOWN_FAULTS = {'starter-config-too-wide': '3'}       # fault -> the check that must catch it
UNIT = 'riley-agentd'
# Fixed expected values (the plan and the lead's M5 install rules).
BINARIES = {'riley-agentd': 'src/agentd/riley-agentd', 'riley-agent': 'src/cli/riley-agent',
            'riley-agent-loop': 'src/loop/riley-agent-loop', 'riley-agent-window': 'src/loop/riley-agent-window'}
TEST_BINARIES = ('riley-test-app', 'riley-window-harness', 'tst_policy')
STARTER_REL = 'share/riley-desktop-agent/starter-config.xml'
MIN_CASES = 20
OUTPUT = 'DP-2'
STARTER_ENDPOINT_URL = 'http://127.0.0.1:8080/v1'
STARTER_ACTIONS = {'see', 'windows'}
STATE_KEYS = {'version', 'state', 'client', 'action', 'since'}
EARLIER_GATES = ('run-policy.sh', 'run-cli.sh', 'run-desktop.sh', 'run-loop.sh')
GATE_TIMEOUT_S = 1200
LISTING = re.compile(r'^(0x[0-9a-fA-F]+) (\S+) (\S+) (-?\d+),(-?\d+) (\d+)x(\d+) "(.*)"$')

FAILS = []


def check(name, ok, detail=''):
    print(f'{"PASS" if ok else "FAIL"} {name}{": " + detail if detail and not ok else ""}', flush=True)
    if not ok:
        FAILS.append(name)
    return ok


def skip(name, why):
    print(f'SKIP {name}: {why}', flush=True)


def sha256(path):
    try:
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()
    except OSError:
        return None


def run(argv, log, env=None, timeout=600):
    with open(log, 'w') as f:
        try:
            return subprocess.run(argv, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                                  timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            f.write(f'\n(timed out after {timeout} s)\n')
            return 124
        except OSError as e:
            f.write(f'\n(could not run {argv[0]}: {e})\n')
            return 127


def systemctl_show():
    r = subprocess.run(['systemctl', '--user', 'show', UNIT, '-p',
                        'LoadState,ActiveState,UnitFileState,InvocationID,FragmentPath,MainPID'],
                       capture_output=True, text=True, timeout=10)
    return dict(line.split('=', 1) for line in r.stdout.splitlines() if '=' in line)


def systemctl_word(verb):
    r = subprocess.run(['systemctl', '--user', verb, UNIT], capture_output=True, text=True, timeout=10)
    return r.stdout.strip()


def protected_files():
    """What a staging run must leave alone, as (path -> sha256 or None)."""
    paths = [REAL_PREFIX / 'bin' / b for b in BINARIES] + [REAL_CONFIG_DIR / 'config.xml',
                                                            REAL_UNIT_DIR / f'{UNIT}.service']
    return {str(p): sha256(p) for p in paths}


def mode(path):
    return stat.S_IMODE(os.lstat(path).st_mode)


def client_kind():
    """The coding agent this gate runs under, from its process ancestry (as the desktop gate finds it)."""
    pid = os.getpid()
    while pid > 1:
        try:
            comm = Path(f'/proc/{pid}/comm').read_text().strip()
            ppid = int(Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            return None
        if comm == 'claude':
            return 'claude-code'
        if comm == 'pi':
            return 'pi'
        pid = ppid
    return None


def ancestors():
    out, pid = set(), os.getpid()
    while pid > 1:
        out.add(pid)
        try:
            pid = int(Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            break
    return out


def hypr(what):
    r = subprocess.run(['hyprctl', what, '-j'], capture_output=True, text=True, timeout=5, check=True)
    return json.loads(r.stdout) if r.stdout.strip() else {}


def unit_lines(text):
    """{section: [(key, value)]} of a unit file."""
    out, section = {}, None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(('#', ';')):
            continue
        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1]
            out.setdefault(section, [])
        elif '=' in line and section:
            k, v = line.split('=', 1)
            out[section].append((k.strip(), v.strip()))
    return out


def values(sections, section, key):
    vals = []
    for k, v in sections.get(section, []):
        if k == key:
            vals += v.split()
    return vals


# --- check 1 -----------------------------------------------------------------------------------------------------
def build(build_dir, plant):
    rc = run(['cmake', '-S', str(REPO), '-B', str(build_dir), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug',
              f'-DRILEY_PLANTED_FAULT={plant}'], SCRATCH / 'configure.log')
    if not check('1 build: cmake configure', rc == 0, f'exit {rc} (see {SCRATCH}/configure.log)'):
        return False
    rc = run(['cmake', '--build', str(build_dir), '-j8'], SCRATCH / 'build.log', timeout=3600)
    ok = check(f'1 build: every default target{" with planted fault " + plant if plant else ""}', rc == 0,
               f'exit {rc} (see {SCRATCH}/build.log)')
    for name, rel in BINARIES.items():
        ok &= check(f'1 build: {rel}', os.access(build_dir / rel, os.X_OK), 'missing')
    ok &= check('1 build: tests/policy/tst_policy', os.access(build_dir / 'tests/policy/tst_policy', os.X_OK),
                'missing')
    return ok


# --- check 2 -----------------------------------------------------------------------------------------------------
def shim_dir():
    """A systemctl first on PATH for staging runs: records the call and fails, so a staging install that reaches
    for systemd is caught (and does nothing)."""
    d = SCRATCH / 'shim'
    d.mkdir(parents=True, exist_ok=True)
    rec = SCRATCH / 'shim-calls.txt'
    if rec.exists():
        rec.unlink()
    shim = d / 'systemctl'
    shim.write_text(f'#!/bin/sh\necho "systemctl $*" >> "{rec}"\nexit 1\n')
    shim.chmod(0o755)
    return d, rec


def run_installer(build_dir, prefix, config_dir, unit_dir, start, log, env=None):
    argv = [str(INSTALLER), '--build', str(build_dir), '--prefix', str(prefix), '--config-dir', str(config_dir),
            '--unit-dir', str(unit_dir)] + (['--start'] if start else [])
    rc = run(argv, log, env=env)
    try:
        out = Path(log).read_text()
    except OSError:
        out = ''
    return rc, out


def check_unit_file(path, prefix):
    if not check('2 unit: file written', path.is_file(), f'{path} missing'):
        return
    r = subprocess.run(['systemd-analyze', '--user', 'verify', str(path)], capture_output=True, text=True,
                       timeout=60)
    (SCRATCH / 'verify.log').write_text(r.stdout + r.stderr)
    named = [l for l in (r.stdout + r.stderr).splitlines() if f'{UNIT}.service' in l]
    check('2 unit: systemd-analyze --user verify', r.returncode == 0 and not named,
          f'exit {r.returncode}: {" | ".join(named)[:300]}')
    text = path.read_text()
    s = unit_lines(text)
    check('2 unit: PartOf=graphical-session.target', 'graphical-session.target' in values(s, 'Unit', 'PartOf'),
          str(values(s, 'Unit', 'PartOf')))
    check('2 unit: After=graphical-session.target', 'graphical-session.target' in values(s, 'Unit', 'After'),
          str(values(s, 'Unit', 'After')))
    execs = [v for k, v in s.get('Service', []) if k == 'ExecStart']
    want = str(prefix / 'bin/riley-agentd')
    check(f'2 unit: ExecStart={want}', len(execs) == 1 and execs[0].split()[0].lstrip('-@:+!') == want, str(execs))
    check('2 unit: Restart=on-failure', values(s, 'Service', 'Restart') == ['on-failure'],
          str(values(s, 'Service', 'Restart')))
    check('2 unit: WantedBy=graphical-session.target',
          'graphical-session.target' in values(s, 'Install', 'WantedBy'), str(values(s, 'Install', 'WantedBy')))
    bad = [l for l in text.splitlines() if re.search(r'inhibit|ipc|namespace', l, re.I) and not l.lstrip().startswith('#')]
    check('2 unit: no sleep inhibitor, no IPC or namespace setting', not bad, ' | '.join(bad)[:300])


def install(build_dir, prefix, real):
    config_dir = REAL_CONFIG_DIR if real else prefix / 'config/riley-desktop-agent'
    unit_dir = REAL_UNIT_DIR if real else prefix / 'systemd/user'
    config = config_dir / 'config.xml'
    had_config = config.exists()
    config_sha = sha256(config)
    env, rec = None, None
    before_show, before_files = systemctl_show(), protected_files()
    if not real:
        d, rec = shim_dir()
        env = dict(os.environ, PATH=f'{d}:{os.environ.get("PATH", "/usr/bin:/bin")}')
    if not INSTALLER.is_file() or not os.access(INSTALLER, os.X_OK):
        check('2 install: tools/install.sh exists and is executable', False, 'missing (the installer is not written yet)')
        return None
    rc, out = run_installer(build_dir, prefix, config_dir, unit_dir, real, SCRATCH / 'install.log', env)
    check(f'2 install: installer exit 0 ({"real, --start" if real else "staging, no --start"})', rc == 0,
          f'exit {rc} (see {SCRATCH}/install.log)')

    for name, rel in BINARIES.items():
        p = prefix / 'bin' / name
        good = p.is_file() and os.access(p, os.X_OK)
        check(f'2 install: {p} executable', good, 'missing or not executable')
        if good:
            check(f'2 install: {name} sha256 equals the build\'s', sha256(p) == sha256(build_dir / rel),
                  f'{sha256(p)} vs {sha256(build_dir / rel)}')
    extra = [n for n in TEST_BINARIES if (prefix / 'bin' / n).exists()]
    check('2 install: no test binary in PREFIX/bin', not extra, ' '.join(extra))

    starter = prefix / STARTER_REL
    if check(f'2 install: starter copy {starter}', starter.is_file(), 'missing'):
        r = subprocess.run(['xmllint', '--noout', '--schema', str(SCHEMA), str(starter)], capture_output=True,
                           text=True)
        check('2 install: starter copy valid to schema/config.xsd', r.returncode == 0, r.stderr.strip()[:300])

    if had_config:
        check(f'2 config: {config} was there before, kept byte for byte', sha256(config) == config_sha,
              'it changed')
        check('2 config: installer says "config: kept "', any(l.startswith('config: kept ') for l in out.splitlines()),
              'no such line')
        if real:
            print(f'INFO {config} was there before the run and was left as it was; check 3 tests the starter copy',
                  flush=True)
            r = subprocess.run(['xmllint', '--noout', '--schema', str(SCHEMA), str(config)], capture_output=True,
                               text=True)
            check(f'2 config: {config} valid to schema/config.xsd', r.returncode == 0, r.stderr.strip()[:300])
    else:
        if check(f'2 config: {config} written', config.is_file(), 'missing'):
            check('2 config: identical to the starter copy', sha256(config) == sha256(starter), 'differs')
            check('2 config: file mode 0600', mode(config) == 0o600, oct(mode(config)))
            check('2 config: folder mode 0700', mode(config_dir) == 0o700, oct(mode(config_dir)))
        check('2 config: installer says "config: wrote "',
              any(l.startswith('config: wrote ') for l in out.splitlines()), 'no such line')

    check_unit_file(unit_dir / f'{UNIT}.service', prefix)

    state = None
    if real:
        check('2 service: is-active = active', systemctl_word('is-active') == 'active', systemctl_word('is-active'))
        check('2 service: is-enabled = enabled', systemctl_word('is-enabled') == 'enabled',
              systemctl_word('is-enabled'))
        show = systemctl_show()
        check('2 service: FragmentPath is the installed unit file',
              show.get('FragmentPath') == str(REAL_UNIT_DIR / f'{UNIT}.service'), show.get('FragmentPath', ''))
        pid = int(show.get('MainPID', '0') or 0)
        try:
            exe = os.readlink(f'/proc/{pid}/exe') if pid > 0 else ''
        except OSError:
            exe = ''
        check('2 service: its main process runs PREFIX/bin/riley-agentd', exe == str(prefix / 'bin/riley-agentd'),
              f'MainPID {pid} exe {exe!r}')
        runtime = Path(os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}')) / 'riley-desktop-agent'
        sock = runtime / 'agentd.sock'
        for _ in range(50):
            if sock.exists():
                break
            subprocess.run(['sleep', '0.1'])
        check(f'2 service: {runtime} mode 0700', runtime.is_dir() and mode(runtime) == 0o700,
              oct(mode(runtime)) if runtime.exists() else 'missing')
        check(f'2 service: {sock} is a socket with mode 0600',
              sock.exists() and stat.S_ISSOCK(os.lstat(sock).st_mode) and mode(sock) == 0o600
              and os.lstat(sock).st_uid == os.getuid(),
              (f'mode {oct(mode(sock))}' if sock.exists() else 'missing'))
        state_file = runtime / 'state.json'
        try:
            text = state_file.read_text()
            state = json.loads(text)
            shape = text.strip().count('\n') == 0 and isinstance(state, dict) and STATE_KEYS <= set(state)
        except (OSError, ValueError):
            state, shape = None, False
        check(f'2 service: {state_file} is one line of JSON with {" ".join(sorted(STATE_KEYS))}', shape,
              'missing or not that shape')
        want = 'stopped' if STOP_COPY.exists() else 'idle'
        check(f'2 service: bar state is {want}', isinstance(state, dict) and state.get('state') == want,
              str(state.get('state') if isinstance(state, dict) else None))
    else:
        calls = rec.read_text().strip() if rec and rec.exists() else ''
        check('2 staging: the installer called no systemctl', not calls, calls[:300])
        check('2 staging: the user unit riley-agentd is as it was', systemctl_show() == before_show,
              f'{before_show} -> {systemctl_show()}')
        after = protected_files()
        changed = [p for p in after if after[p] != before_files[p]]
        check('2 staging: nothing under ~/.local/bin, ~/.config/riley-desktop-agent or the user units changed',
              not changed, ' '.join(changed))
    return state


def keep_check(build_dir):
    """2k: an existing config.xml is never overwritten (staging prefix, no --start, systemctl shim)."""
    prefix = STAGE / 'keep'
    if prefix.exists():
        shutil.rmtree(prefix)
    config_dir = prefix / 'config/riley-desktop-agent'
    config_dir.mkdir(parents=True, mode=0o700)
    config = config_dir / 'config.xml'
    config.write_text('<?xml version="1.0" encoding="UTF-8"?>\n<!-- made up by the install gate: an existing config '
                      'that must be kept -->\n<riley-desktop-agent version="1"/>\n')
    before = sha256(config)
    d, rec = shim_dir()
    env = dict(os.environ, PATH=f'{d}:{os.environ.get("PATH", "/usr/bin:/bin")}')
    rc, out = run_installer(build_dir, prefix, config_dir, prefix / 'systemd/user', False, SCRATCH / 'install-keep.log',
                            env)
    check('2k keep: installer exit 0 with a config.xml already there', rc == 0, f'exit {rc}')
    check('2k keep: the existing config.xml is unchanged', sha256(config) == before, 'it changed')
    check('2k keep: installer says "config: kept "', any(l.startswith('config: kept ') for l in out.splitlines()),
          'no such line')
    calls = rec.read_text().strip() if rec.exists() else ''
    check('2k keep: no systemctl call', not calls, calls[:300])
    shutil.rmtree(prefix)


# --- check 3 -----------------------------------------------------------------------------------------------------
def starter_cases(build_dir, starter):
    root = ET.parse(CASES).getroot()
    cases = root.findall('case')
    check(f'3 table: {len(cases)} cases (at least {MIN_CASES})', len(cases) >= MIN_CASES)
    declared = {r.get('code') for r in root.iter('reason')}
    used = {c.get('reason') for c in cases}
    check('3 table: every declared reason used, every used reason declared', declared == used,
          f'undeclared {sorted(used - declared)} unused {sorted(declared - used)}')
    targeted = [c.get('id') for c in cases if c.get('targets') == 'starter-config-too-wide']
    check('3 table: a case targets starter-config-too-wide', bool(targeted))
    check('3 table: every case uses config="starter"', all(c.get('config') == 'starter' for c in cases))

    if not starter.is_file():
        check('3 starter: the installed starter copy exists', False, f'{starter} missing')
        return set(), targeted
    shape(starter)

    driver = build_dir / 'tests/policy/tst_policy'
    if not check('3 driver: tests/policy/tst_policy built', os.access(driver, os.X_OK), 'missing'):
        return set(), targeted
    out = SCRATCH / 'tst_policy-starter.txt'
    env = dict(os.environ, RILEY_POLICY_CASES=str(CASES), RILEY_POLICY_STARTER_CONFIG=str(starter),
               RILEY_POLICY_MIN_CASES=str(MIN_CASES))
    subprocess.run([str(driver), '-o', f'{out},txt'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=120)
    lines = out.read_text().splitlines() if out.exists() else []
    if any(l.startswith('FAIL!  : tst_Policy::initTestCase') for l in lines):
        i = next(i for i, l in enumerate(lines) if l.startswith('FAIL!  : tst_Policy::initTestCase'))
        check('3 driver: initTestCase', False, ' '.join(x.strip() for x in lines[i:i + 2]))
    loaded = any(l.startswith('PASS   : tst_Policy::schema(starter config)') for l in lines)
    check('3 loader: the engine loads the starter config Ok', loaded,
          next((l for l in lines if 'tst_Policy::schema(' in l), 'no schema row'))
    failed, rows = set(), 0
    for i, l in enumerate(lines):
        m = re.match(r'^(PASS   |FAIL!  ): tst_Policy::decide\(([^)]*)\)', l)
        if not m:
            continue
        rows += 1
        cid = m.group(2)
        if m.group(1).startswith('PASS'):
            check(f'3 case {cid}', True)
        else:
            failed.add(cid)
            check(f'3 case {cid}', False, ' '.join(x.strip() for x in lines[i:i + 3])[:400])
    check(f'3 table: case rows run {rows} of {len(cases)}', rows == len(cases))
    return failed, targeted


def shape(starter):
    try:
        root = ET.parse(starter).getroot()
    except ET.ParseError as e:
        check('3 shape: the starter copy parses', False, str(e))
        return
    eps = root.findall('./endpoints/endpoint')
    check(f'3 shape: one endpoint, local, {STARTER_ENDPOINT_URL}',
          len(eps) == 1 and eps[0].get('location') == 'local' and eps[0].get('base-url') == STARTER_ENDPOINT_URL,
          str([e.attrib for e in eps]))
    keys = root.findall('.//key-env') + root.findall('.//key-file')
    check('3 shape: no key-env, no key-file', not keys, f'{len(keys)} found')
    clients = {c.get('kind'): c.attrib for c in root.findall('./clients/client')}
    check('3 shape: clients own-loop, claude-code, pi and no other', set(clients) == {'own-loop', 'claude-code', 'pi'}
          and len(root.findall('./clients/client')) == 3, str(sorted(clients)))
    own = clients.get('own-loop', {})
    check('3 shape: own-loop names the local endpoint',
          bool(eps) and own.get('endpoint') == eps[0].get('name') and own.get('location', 'local') == 'local',
          str(own))
    check('3 shape: claude-code location remote', clients.get('claude-code', {}).get('location') == 'remote',
          str(clients.get('claude-code')))
    check('3 shape: pi location remote', clients.get('pi', {}).get('location') == 'remote', str(clients.get('pi')))
    allows = root.findall('./permissions/allow')
    check('3 shape: exactly two permissions', len(allows) == 2, str([a.get('id') for a in allows]))
    wide = [a.get('id') for a in allows if not set((a.get('actions') or '').split()) <= STARTER_ACTIONS]
    check('3 shape: every permission\'s actions within "see windows"', not wide, ' '.join(map(str, wide)))
    check('3 shape: no permission for own-loop', not [a for a in allows if a.get('client') == 'own-loop'])
    check('3 shape: no named hook editors', not root.findall('./named-hook-editors/editor'))


# --- check 4 -----------------------------------------------------------------------------------------------------
def windows_check(prefix):
    kind = client_kind()
    if not check('4 client: this gate runs under Claude Code or Pi', kind is not None,
                 'neither claude nor pi among its ancestors'):
        return
    try:
        mons = {m['id']: m['name'] for m in hypr('monitors')}
        before_clients = hypr('clients')
        cursor_before = hypr('cursorpos')
        active_before = hypr('activewindow').get('address')
    except (subprocess.SubprocessError, ValueError, KeyError) as e:
        check('4 hyprctl: monitors, clients, cursorpos, activewindow', False, str(e))
        return
    log_before = LOG_XML.read_text(errors='replace').splitlines() if LOG_XML.exists() else []
    r = subprocess.run([str(prefix / 'bin/riley-agent'), 'windows'], capture_output=True, text=True, timeout=30,
                       stdin=subprocess.DEVNULL)
    (SCRATCH / 'windows.txt').write_text(r.stdout + '\n--- stderr\n' + r.stderr)
    after_clients = hypr('clients')
    lines = r.stdout.splitlines()
    first = lines[0].strip() if lines else ''
    m = re.match(r'^ok (\d+) windows?$', first)
    if not check('4 windows: exit 0 and first line "ok N windows"', r.returncode == 0 and m is not None,
                 f'exit {r.returncode}, first line {first[:160]!r}'):
        return
    body = [l for l in lines[1:] if l.strip()]
    check(f'4 windows: {m.group(1)} windows said, {len(body)} listed', int(m.group(1)) == len(body))
    where = {}
    for c in before_clients + after_clients:
        where.setdefault(c.get('address'), set()).add(mons.get(c.get('monitor'), f'monitor {c.get("monitor")}'))
    bad = []
    for l in body:
        lm = LISTING.match(l)
        if not lm:
            bad.append(f'not "ADDR class monitor x,y wxh "title"": {l[:120]!r}')
            continue
        addr, monitor = lm.group(1), lm.group(3)
        if monitor != OUTPUT:
            bad.append(f'{addr} says monitor {monitor}')
        if where.get(addr) != {OUTPUT}:
            bad.append(f'{addr} is on {sorted(where.get(addr, {"no monitor (unknown window)"}))} by hyprctl')
    check(f'4 windows: every listed window is on {OUTPUT}', not bad, '; '.join(bad)[:500])
    mine = ancestors()
    own_terms = {c.get('address') for c in before_clients if c.get('pid') in mine}
    listed = {LISTING.match(l).group(1) for l in body if LISTING.match(l)}
    check('4 windows: the terminal this gate runs in is not listed (R6c, agent terminal)', not (own_terms & listed),
          ' '.join(sorted(own_terms & listed)))
    log_after = LOG_XML.read_text(errors='replace').splitlines() if LOG_XML.exists() else []
    new = log_after[len(log_before):] if log_after[:len(log_before)] == log_before else log_after
    hit = [l for l in new if f'client="{kind}"' in l and 'action="windows"' in l]
    check(f'4 log: a new log.xml line client="{kind}" action="windows"', bool(hit), f'{len(new)} new lines, none such')
    check('4 read-only: the cursor did not move', hypr('cursorpos') == cursor_before,
          f'{cursor_before} -> {hypr("cursorpos")}')
    check('4 read-only: the active window did not change', hypr('activewindow').get('address') == active_before)


# --- check 5 -----------------------------------------------------------------------------------------------------
def earlier_gates(prefix):
    for g in EARLIER_GATES:
        log = SCRATCH / f'{g[:-3]}-installed.log'
        rc = run([str(REPO / 'tests' / g), '--installed', str(prefix)], log, timeout=GATE_TIMEOUT_S)
        lines = log.read_text(errors='replace').splitlines() if log.exists() else []
        last = lines[-1].strip() if lines else ''
        fails = [l for l in lines if l.startswith('FAIL ')]
        check(f'5 tests/{g} --installed {prefix}', rc == 0 and last == 'PASS',
              f'exit {rc}, last line {last!r}, {len(fails)} FAIL lines (see {log}): ' + ' | '.join(fails[:3])[:400])


def main():
    ap = argparse.ArgumentParser(description='M5 install gate (run it through tests/run-install.sh)')
    ap.add_argument('--plant', default='')
    ap.add_argument('--prefix', default='')
    a = ap.parse_args()
    if a.plant and a.plant not in KNOWN_FAULTS:
        print(f'unknown planted fault: {a.plant} (known: {", ".join(KNOWN_FAULTS)})')
        return 2
    prefix = Path(os.path.abspath(os.path.expanduser(a.prefix))) if a.prefix else (STAGE / 'plant' if a.plant
                                                                                   else REAL_PREFIX)
    real = prefix == REAL_PREFIX
    if real and a.plant:
        print('a planted fault installs only to the staging prefix, never to ~/.local')
        return 2
    if not real and not str(prefix).startswith(str(STAGE) + '/'):
        print(f'the prefix must be {REAL_PREFIX} or a folder under {STAGE}')
        return 2
    build_dir = BUILD_FAULT if a.plant else BUILD
    SCRATCH.mkdir(parents=True, exist_ok=True)
    STAGE.mkdir(parents=True, exist_ok=True)
    if not real and prefix.exists():
        shutil.rmtree(prefix)
    print(f'INFO mode {"real" if real else "staging"}, prefix {prefix}, build {build_dir}', flush=True)

    failed, targeted = set(), []
    if build(build_dir, a.plant):
        state = install(build_dir, prefix, real)
        keep_check(build_dir)
        failed, targeted = starter_cases(build_dir, prefix / STARTER_REL)
        if real:
            windows_check(prefix)
            pid_before = systemctl_show().get('MainPID')
            stop_before = STOP_COPY.exists()
            earlier_gates(prefix)
            show = systemctl_show()
            check('6 service: still active after the earlier gates', show.get('ActiveState') == 'active',
                  show.get('ActiveState', ''))
            check('6 service: same main PID', show.get('MainPID') == pid_before, f'{pid_before} -> {show.get("MainPID")}')
            check('6 service: the persisted stop copy as before', STOP_COPY.exists() == stop_before)
            runtime = Path(os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}')) / 'riley-desktop-agent'
            try:
                now = json.loads((runtime / 'state.json').read_text()).get('state')
            except (OSError, ValueError, AttributeError):
                now = None
            want = state.get('state') if isinstance(state, dict) else None
            check(f'6 service: bar state still {want}', now == want and want is not None, str(now))
        else:
            skip('4 windows from the installed service', 'staging run: no service is installed or started')
            skip('5 earlier gates on the installed build', 'staging run')
            skip('6 service after the earlier gates', 'staging run')
        if not real and prefix.exists():
            shutil.rmtree(prefix)

    if a.plant:
        missed = [t for t in targeted if t not in failed]
        if failed and targeted and not missed:
            print(f'PLANTED FAULT {a.plant} CAUGHT by: ' + ', '.join(f'case {c}' for c in sorted(failed)))
        else:
            print(f'PLANTED FAULT {a.plant} NOT CAUGHT (targeted cases still passing: {" ".join(missed) or "none ran"})')
    if not FAILS:
        print('PASS')
        return 0
    print(f'FAIL ({len(FAILS)} checks failed)')
    return 1


if __name__ == '__main__':
    sys.exit(main())
