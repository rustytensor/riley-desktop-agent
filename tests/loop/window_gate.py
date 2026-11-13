# SPDX-License-Identifier: GPL-3.0-or-later
"""M4 gate, check (f): the window's Re-enable, and nothing else in the window, resumes. No desktop.

Started by tests/loop/gate.py (tests/run-loop.sh) INSIDE a bwrap sandbox built like the M3 gate's (tests/run-cli.sh):
own process, network, IPC and UTS namespaces; a fresh /dev (no /dev/uinput); empty tmpfs over /tmp, /home and
$XDG_RUNTIME_DIR (no Wayland, X, session bus or real Hyprland socket; Marc's home is not visible); a clean
environment; the repo and the build dir read-only. It refuses to run anywhere else (the M3 sandbox checks, reused from
tests/cli/gate.py). Inside the private process namespace the gate's own Claude Code (or Pi) is not among anyone's
ancestors, so a process started straight from this script is "the user" to the service, as Marc's own terminal is.
Every Qt program here runs with QT_QPA_PLATFORM=offscreen (and the software Qt Quick renderer): nothing is shown.

Interface this check expects (the builder makes the window follow it)
---------------------------------------------------------------------
riley-agent-window (build/src/loop/riley-agent-window; comm "riley-agent-win", the kernel keeps 15 characters)
    The Qt Quick window. For each task it starts riley-agent-loop as its child and relays Stop and the safe word over
    the child's standard input. Its Re-enable button (objectName reenableButton) is the ONLY way it resumes: no
    command-line flag, standard-input line, socket, D-Bus name or file makes it send resume.
    --help (or -h): the usage on standard output, exit 0, without contacting the service; it names every option the
    window accepts, each as "--name" or "--name VALUE" (VALUE in capitals: FILE, PATH, NAME, N ...).
    --list-ui: with QT_QPA_PLATFORM=offscreen, builds the window from its QML, prints one line per item that has an
    objectName ("<objectName> <QML type>"), exits 0 without contacting the service or a model. The items include
    taskField, transcriptView, stopButton and reenableButton.
    An option it does not accept: exit 2 (bad usage) at once.
    With the service stopped, reenableButton is visible and enabled within 5 s of the window's start.
riley::createWindow (src/loop/window.h, in the library target riley_window, which brings its QML module with it):
        QQuickWindow *riley::createWindow(QQmlApplicationEngine *engine, const QStringList &arguments,
                                          QString *problem);
    arguments as QCoreApplication::arguments() (program name first). Builds the window exactly as riley-agent-window
    does: parses its options, makes the controller, loads Main.qml. Returns the window (owned by the engine), or
    nullptr with *problem set. riley-agent-window's main() is this call and app.exec() (plus --help and --list-ui).
riley-window-harness (tests/loop/harness, test use only, never installed): createWindow with its own arguments,
    show offscreen, wait for reenableButton visible and enabled (5 s), one Qt Test mouse click on its centre, run 3 s
    more; prints "pressed reenableButton", exit 0 (else "failed <what>", exit 4). Its comm "riley-window-ha" is none
    of claude, pi, codex, riley-agent-loo: the service must decide by ancestry, not by the window's name.
The service (R0, lead decision R2=A and the window-resume design): resume is refused (resume-needs-user) when any
    ancestor of the caller has comm claude, pi, codex or riley-agent-loo (riley-agent-loop cut to 15 characters);
    any other caller is the user and is allowed, even with no config.

Expected results, worked out by hand from the brief, docs/files.md and the lead's decisions, before the window code
-----------------------------------------------------------------------------------------------------------------------
One private riley-agentd (home /tmp/riley-window-gate/home, a valid config at its default path), then in order:
 step                                   expected                                   deciding rule and why
 -------------------------------------  -----------------------------------------  ---------------------------------
 ui   riley-agent-window --list-ui      exit 0 within 15 s; lists taskField,        the interface above; --list-ui
                                        transcriptView, stopButton, reenableButton; never contacts the service
                                        no new service log line
 s0   riley-agent stop (plain caller)   "ok", exit 0; log <control action=stop      stop works for anyone (brief:
                                        result=done>; bar stopped                   Stop); bar: docs/files.md
 f4   riley-agent-window --help         exit 0 within 10 s; names --list-ui and     the interface above
                                        --help; no new service log line
 f4   no option; then each option the   no <control action=resume> line at all;    R2=A design: the button is the
      help names (VALUE: FILE -> the    bar stays stopped; the service runs. The   only way; a resume sent by the
      config, PATH -> the socket, NAME  window with no option is still running     window would be allowed (it is
      -> mock-local, N -> 5, else x),   after 2.5 s (it really started). Each run  the user), so any resume line
      stdin "resume", "reenable",       is ended by its PID after 2.5 s.           here is the window resuming
      "re-enable", "enable", "start"
 f4   every "--name" string in the      either refused as bad usage (exit 2 within  an option the window accepts
      riley-agent-window binary not     2.5 s, with and without a value) or named  must be in --help (the brief:
      named by --help                   by --help; and none resumes (as above)     no hidden way to resume)
 f2   riley-window-harness under a      harness: "pressed reenableButton", exit 0;  R0: claude among the ancestors
      parent with comm claude           exactly one new <control action=resume      -> resume-needs-user. The press
                                        result=refused client=claude-code>; bar     must reach the service (a line
                                        stays stopped                               exists), or this proves nothing
 f3   riley-agent resume under a parent first line "refused resume-needs-user",    R0: riley-agent-loo among the
      with comm riley-agent-loo         exit 1; one new <control action=resume      ancestors. client not checked
                                        result=refused>; bar stays stopped          (the service may name it
                                                                                    own-loop or unknown)
 f1   riley-window-harness, plain       harness: "pressed reenableButton", exit 0;  R0 passes: no agent and not the
      parent (hidden ancestry)          exactly one new <control action=resume      loop among the ancestors -> the
                                        result=done>, client unknown (or empty);    user; docs/files.md: an allowed
                                        bar idle                                    resume goes back to idle
f1 runs last, so every earlier step starts from the stopped state; f2, f3 and f1 each check it first and stop the
service again (with a check that says so) if an earlier step wrongly resumed it.

Prints one PASS or FAIL line per check (names start "window-resume:" or "ui:"), then a final PASS or FAIL line.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location('riley_cli_gate', REPO / 'tests/cli/gate.py')
cli_gate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cli_gate)

UI_OBJECTS = ['taskField', 'transcriptView', 'stopButton', 'reenableButton']
STDIN_LINES = b'resume\nreenable\nre-enable\nenable\nstart\n'
RUN_S = 2.5
PRESSED = 'pressed reenableButton'
WORK = Path('/tmp/riley-window-gate')

FAILS = 0


def report(ok, ident, text=''):
    global FAILS
    print(('PASS ' if ok else 'FAIL ') + ident + (': ' + str(text) if text else ''), flush=True)
    if not ok:
        FAILS += 1
    return ok


CONFIG = '''<?xml version="1.0" encoding="UTF-8"?>
<!-- Written by tests/loop/window_gate.py: a valid config for the window check. Nothing listens on its endpoint. -->
<riley-desktop-agent version="1">
  <endpoints>
    <endpoint name="mock-local" base-url="http://127.0.0.1:9/v1" model="mock-model" location="local"/>
  </endpoints>
  <clients>
    <client kind="own-loop" endpoint="mock-local"/>
  </clients>
  <remote-allow>
    <window id="gate-remote-testapp" class="org.riley.testapp" title="Riley test app" monitor="DP-2"/>
  </remote-allow>
  <permissions>
    <allow id="loop-app" client="own-loop" actions="windows see tree click type">
      <window class="org.riley.testapp" title="Riley test app" monitor="DP-2"/>
    </allow>
  </permissions>
</riley-desktop-agent>
'''


class Run:
    def __init__(self, rc, out, err, alive):
        self.rc, self.out, self.err, self.alive = rc, out, err, alive

    @property
    def lines(self):
        return [l for l in self.out.splitlines() if l.strip()]


def run_for(argv, env, cwd, seconds, stdin_data=b''):
    """Start argv, feed stdin, let it run `seconds` (or until it exits), then end it by its PID."""
    p = subprocess.Popen(argv, env=env, cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    try:
        if stdin_data:
            p.stdin.write(stdin_data)
            p.stdin.flush()
    except OSError:
        pass
    deadline = time.monotonic() + seconds
    while p.poll() is None and time.monotonic() < deadline:
        time.sleep(0.05)
    alive = p.poll() is None
    if alive:
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait(timeout=5)
    try:
        p.stdin.close()
    except OSError:
        pass
    out = p.stdout.read().decode('utf-8', 'replace')
    err = p.stderr.read().decode('utf-8', 'replace')
    return Run(None if alive else p.returncode, out, err, alive)


def new_lines(inst, offset):
    return [cli_gate.parse_log_line(l) for l in inst.log_since(offset)]


def resume_lines(lines):
    return [e for e in lines if e is not None and e.tag == 'control' and e.get('action') == 'resume']


def help_options(text):
    """{option: value placeholder or None} for every --name in the help text."""
    found = {}
    for m in re.finditer(r'(?<![\w-])(--[a-z][a-z0-9-]*)(?:[ =]([A-Z][A-Z0-9_]*)\b)?', text):
        name, value = m.group(1), m.group(2)
        if name not in found or (found[name] is None and value):
            found[name] = value
    return found


def binary_options(path):
    data = Path(path).read_bytes()
    names = {m.group(1).decode() for m in re.finditer(rb'(?<![\x21-\x7e])(--[a-z][a-z0-9-]{1,40})(?=\x00)', data)}
    for m in re.finditer(rb'-\x00-\x00((?:[a-z0-9-]\x00){2,41})\x00\x00', data):
        s = m.group(1).decode('utf-16-le')
        if re.fullmatch(r'[a-z][a-z0-9-]*', s):
            names.add('--' + s)
    return names


def value_for(placeholder, config, socket_path):
    return {'FILE': config, 'PATH': socket_path, 'NAME': 'mock-local', 'N': '5', 'NUMBER': '5'}.get(placeholder, 'x')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    runtime = os.environ.get('XDG_RUNTIME_DIR', '')
    problems = cli_gate.sandbox_problems(runtime)
    report(not problems, 'window-resume: sandbox: no uinput, no Wayland/X/bus, no real Hyprland, private namespaces, '
           'no coding agent among this process\'s ancestors', '; '.join(problems))
    if problems:
        print('FAIL (the sandbox is not safe; nothing was started)')
        return 1

    build = Path(a.build)
    agentd, cli = build / 'src/agentd/riley-agentd', build / 'src/cli/riley-agent'
    window, harness = build / 'src/loop/riley-agent-window', build / 'tests/loop/harness/riley-window-harness'
    have = {}
    for exe in (agentd, cli, window, harness):
        have[exe] = report(os.access(exe, os.X_OK), f'window-resume: binary {exe.relative_to(build)}',
                           '' if os.access(exe, os.X_OK) else 'missing')
    if not (have[agentd] and have[cli]):
        print(f'FAIL ({FAILS} checks failed)')
        return 1

    bindir = WORK / 'bin'
    bindir.mkdir(parents=True)
    for name in ('claude', 'riley-agent-loop'):   # launcher copies: the service sees comm claude / riley-agent-loo
        shutil.copy2('/usr/bin/timeout', bindir / name)
    home = WORK / 'home'
    cdir = home / '.config/riley-desktop-agent'
    cdir.mkdir(parents=True, mode=0o700)
    config = cdir / 'config.xml'
    config.write_text(CONFIG)
    os.chmod(config, 0o600)
    r = subprocess.run(['xmllint', '--noout', '--schema', str(REPO / 'schema/config.xsd'), str(config)],
                       capture_output=True, text=True)
    report(r.returncode == 0, 'window-resume: the config is valid to schema/config.xsd', r.stderr.strip()[:200])

    inst = cli_gate.Instance('window', str(home), str(agentd), runtime, a.out)
    qt_env = dict(inst.env, QT_QPA_PLATFORM='offscreen', QT_QUICK_BACKEND='software',
                  XDG_CACHE_HOME=str(home / '.cache'))
    problem = inst.start()
    if not report(problem is None, 'window-resume: the private service starts', problem or ''):
        print(f'FAIL ({FAILS} checks failed)')
        return 1
    try:
        steps(inst, qt_env, cli, window, harness, have, bindir, config)
    finally:
        inst.kill(signal.SIGTERM)
    if FAILS == 0:
        print('PASS')
        return 0
    print(f'FAIL ({FAILS} checks failed)')
    return 1


def steps(inst, env, cli, window, harness, have, bindir, config):
    def bar():
        return cli_gate.read_bar(inst.bar)

    def still_stopped(ident, before):
        lines = new_lines(inst, before)
        resumed = resume_lines(lines)
        running = inst.proc.poll() is None
        if not report(not resumed and bar() == 'stopped' and running, ident,
                      f'{len(resumed)} resume lines, bar {bar()!r}, service {"running" if running else "gone"}'):
            subprocess.run([str(cli), 'stop'], env=inst.env, cwd=inst.home, stdin=subprocess.DEVNULL,
                           capture_output=True, timeout=30)    # the next run starts stopped again

    def ensure_stopped(step):
        """Each of f2, f3, f1 starts from the stopped state, even when an earlier step wrongly resumed."""
        if bar() == 'stopped':
            return True
        subprocess.run([str(cli), 'stop'], env=inst.env, cwd=inst.home, stdin=subprocess.DEVNULL,
                       capture_output=True, timeout=30)
        return report(bar() == 'stopped', f'window-resume: {step}: the service is stopped again before it',
                      f'bar {bar()!r}')

    # ui: --list-ui, offscreen, never contacting the service
    if have[window]:
        before = inst.log_size()
        run = run_for([str(window), '--list-ui'], env, inst.home, 15)
        names = {l.split()[0] for l in run.lines}
        missing = [n for n in UI_OBJECTS if n not in names]
        report(run.rc == 0 and not missing and not inst.log_since(before),
               'ui: riley-agent-window --list-ui lists taskField, transcriptView, stopButton, reenableButton (offscreen, '
               'exit 0 within 15 s, no service contact)',
               f'exit {"none within 15 s" if run.alive else run.rc}, missing {missing}, '
               f'{len(inst.log_since(before))} log lines')

    # s0: stop, from a plain caller
    before = inst.log_size()
    r = subprocess.run([str(cli), 'stop'], env=inst.env, cwd=inst.home, stdin=subprocess.DEVNULL,
                       capture_output=True, text=True, timeout=30)
    first = (r.stdout.splitlines() or [''])[0]
    ctl = [e for e in new_lines(inst, before) if e is not None and e.tag == 'control' and e.get('action') == 'stop']
    report(r.returncode == 0 and first.split()[:1] == ['ok'] and len(ctl) == 1 and ctl[0].get('result') == 'done'
           and bar() == 'stopped', 'window-resume: s0: riley-agent stop -> ok, log control stop done, bar stopped',
           f'exit {r.returncode}, first line {first[:80]!r}, {len(ctl)} stop lines, bar {bar()!r}')
    if bar() != 'stopped':
        report(False, 'window-resume: the service is stopped before the window checks', 'it is not; f1-f4 skipped')
        return

    # f4: --help, no option, every option, every --name string in the binary
    if have[window]:
        before = inst.log_size()
        run = run_for([str(window), '--help'], env, inst.home, 10)
        opts = help_options(run.out)
        report(run.rc == 0 and '--list-ui' in opts and '--help' in opts and not inst.log_since(before),
               'window-resume: f4: riley-agent-window --help (exit 0, names --help and --list-ui, no service contact)',
               f'exit {"none within 10 s" if run.alive else run.rc}, options {sorted(opts)}, '
               f'{len(inst.log_since(before))} log lines')
        before = inst.log_size()
        run = run_for([str(window)], env, inst.home, RUN_S, STDIN_LINES)
        report(run.alive, 'window-resume: f4: riley-agent-window with no option starts and runs (2.5 s)',
               f'exit {run.rc}: {(run.err or run.out).strip()[-160:]!r}')
        still_stopped('window-resume: f4: no option, stdin "resume" and the like: no resume, bar stays stopped', before)
        for name, placeholder in sorted(opts.items()):
            if name in ('--help', '-h', '--list-ui'):
                continue
            argv = [str(window), name] + ([value_for(placeholder, str(config), inst.socket)] if placeholder else [])
            before = inst.log_size()
            run_for(argv, env, inst.home, RUN_S, STDIN_LINES)
            still_stopped(f'window-resume: f4: {" ".join(argv[1:])}: no resume, bar stays stopped', before)
        hidden = sorted(binary_options(window) - set(opts))
        for name in hidden:
            accepted = []
            for argv in ([str(window), name], [str(window), name, str(config)]):
                before = inst.log_size()
                run = run_for(argv, env, inst.home, RUN_S, STDIN_LINES)
                if not (run.rc == 2):
                    accepted.append(' '.join(argv[1:]) + (' (still running)' if run.alive else f' (exit {run.rc})'))
                still_stopped(f'window-resume: f4: {" ".join(argv[1:])} (in the binary, not in --help): no resume, '
                              f'bar stays stopped', before)
            report(not accepted, f'window-resume: f4: {name} is in the binary but not in --help: refused as bad usage '
                   '(exit 2)', '; '.join(accepted))
        print(f'INFO window-resume: f4: options from --help {sorted(opts)}; --names in the binary not in --help '
              f'{hidden}', flush=True)

    # f2: the harness under a parent named claude
    if have[harness] and ensure_stopped('f2'):
        before = inst.log_size()
        run = run_for([str(bindir / 'claude'), '30', str(harness)], env, inst.home, 20)
        res = resume_lines(new_lines(inst, before))
        report(run.rc == 0 and PRESSED in run.lines and len(res) == 1 and res[0].get('result') == 'refused'
               and res[0].get('client') == 'claude-code' and bar() == 'stopped',
               'window-resume: f2: Re-enable pressed under claude -> refused (R0 resume-needs-user), bar stays stopped',
               f'harness exit {run.rc}, {run.lines[-1:] or ["no output"]}, resume lines '
               f'{[(e.get("client"), e.get("result")) for e in res]}, bar {bar()!r}')

    # f3: resume from under riley-agent-loop
    if not ensure_stopped('f3'):
        return
    before = inst.log_size()
    r = subprocess.run([str(bindir / 'riley-agent-loop'), '30', str(cli), 'resume'], env=inst.env, cwd=inst.home,
                       stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=40)
    first = (r.stdout.splitlines() or [''])[0]
    res = resume_lines(new_lines(inst, before))
    report(r.returncode == 1 and first.split()[:2] == ['refused', 'resume-needs-user'] and len(res) == 1
           and res[0].get('result') == 'refused' and bar() == 'stopped',
           'window-resume: f3: resume under riley-agent-loop -> refused resume-needs-user (R0), bar stays stopped',
           f'exit {r.returncode}, first line {first[:80]!r}, resume lines '
           f'{[(e.get("client"), e.get("result")) for e in res]}, bar {bar()!r}')

    # f1: the harness from a plain parent: the user pressing Re-enable
    if have[harness] and ensure_stopped('f1'):
        before = inst.log_size()
        run = run_for([str(harness)], env, inst.home, 20)
        res = resume_lines(new_lines(inst, before))
        report(run.rc == 0 and PRESSED in run.lines and len(res) == 1 and res[0].get('result') == 'done'
               and (res[0].get('client') or '') in ('', 'unknown') and bar() == 'idle',
               'window-resume: f1: Re-enable pressed by the user -> control resume done, bar idle',
               f'harness exit {run.rc}, {run.lines[-1:] or ["no output"]}, resume lines '
               f'{[(e.get("client"), e.get("result")) for e in res]}, bar {bar()!r}')


if __name__ == '__main__':
    sys.exit(main())
