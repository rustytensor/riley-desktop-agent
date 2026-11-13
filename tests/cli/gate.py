# SPDX-License-Identifier: GPL-3.0-or-later
#
# M3 gate driver: the command line for coding agents. Started by tests/run-cli.sh INSIDE a bwrap sandbox (own
# process and network namespaces, no /dev/uinput, no Wayland, X or session bus, no real Hyprland socket, a private
# $XDG_RUNTIME_DIR and home). It refuses to run anywhere else.
#
# It serves a fake Hyprland IPC socket from the <desk> in tests/cli/cases.xml, starts a private riley-agentd per
# <instance> (each with its own home, so config, log, hash secret and stop state live there, at their default
# paths), and calls riley-agent as Claude Code, Pi, Codex (children of launcher copies named claude, pi, codex) or
# from a plain shell. Each call is checked against the expected result written by hand in tests/cli/cases.xml:
# the first line and exit code, plain one-result output, the service log line, and that nothing was dispatched to
# Hyprland after a refusal. The table itself is checked against the M1 table (tests/policy/cases.xml).
#
# Two parts, each in its own bwrap sandbox started by tests/run-cli.sh:
#   --part main     checks 1-7 (the table's <instance>s); the sandbox above.
#   --part sandbox  checks 8 and 9 (the table's <sandbox-example>): a "decoy host" sandbox where /dev/uinput (bound
#                   to /dev/null), a Wayland and an X socket (bound by this script, never served) and the fake
#                   Hyprland's folder all exist, so the documented wrapper tools/riley-sandbox has something to hide.
#
# Prints one PASS or FAIL line per check, then "PART <name>: PASS" or "PART <name>: FAIL (n checks failed)";
# exits 0 only on PASS. tests/run-cli.sh prints the final PASS or FAIL line.
import argparse
import json
import os
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET

SUBCOMMANDS = ["see", "tree", "windows", "point", "click", "type", "key", "launch", "close",
               "stop", "segfault", "resume", "help"]
WINDOWED = {"see", "tree", "point", "click", "type", "key", "close"}
CALLER_CLIENT = {"claude": "claude-code", "pi": "pi", "codex": "codex", "shell": ""}
LAUNCHERS = ["claude", "pi", "codex"]
PROCESS_AGENT = {"claude": "claude-code", "pi": "pi", "codex": "codex", "": ""}
FAKE_SIGNATURE = "riley-cli-gate-fake-hyprland"
CALL_TIMEOUT_S = 30
RESULT_RE = re.compile(r"^(ok|stopped|refused [a-z][a-z0-9-]*|failed [a-z][a-z0-9-]*)( |:|$)")
EXIT_FOR = {"ok": 0, "refused": 1, "stopped": 3, "failed": 4}

FAILS = 0
FAILED_IDS = set()


def report(ok, ident, text):
    global FAILS
    print(("PASS " if ok else "FAIL ") + ident + (": " + text if text else ""), flush=True)
    if not ok:
        FAILS += 1
        FAILED_IDS.add(ident)


# --- the sandbox must really be one ------------------------------------------------------------------------------

def comm_and_ppid(pid):
    with open("/proc/%d/stat" % pid, "rb") as f:
        s = f.read().decode("utf-8", "replace")
    comm = s[s.index("(") + 1:s.rindex(")")]
    ppid = int(s[s.rindex(")") + 2:].split()[1])
    return comm, ppid


def sandbox_problems(runtime, decoy_uinput=False):
    """decoy_uinput: /dev/uinput must exist and be the null device (check 8's decoy host), not be absent."""
    problems = []
    for name in ("WAYLAND_DISPLAY", "DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "XDG_SESSION_TYPE"):
        if os.environ.get(name):
            problems.append("%s is set" % name)
    if os.environ.get("HYPRLAND_INSTANCE_SIGNATURE") != FAKE_SIGNATURE:
        problems.append("HYPRLAND_INSTANCE_SIGNATURE is not the gate's fake one")
    if decoy_uinput:
        try:
            st, null = os.stat("/dev/uinput"), os.stat("/dev/null")
            if not (stat.S_ISCHR(st.st_mode) and st.st_rdev == null.st_rdev):
                problems.append("/dev/uinput is not the /dev/null decoy")
        except OSError as e:
            problems.append("/dev/uinput decoy missing: %s" % e)
    elif os.path.exists("/dev/uinput"):
        problems.append("/dev/uinput exists")
    if os.path.exists("/tmp/.X11-unix"):
        problems.append("/tmp/.X11-unix exists")
    if not runtime or not os.path.isdir(runtime):
        problems.append("XDG_RUNTIME_DIR missing")
    elif os.listdir(runtime):
        problems.append("XDG_RUNTIME_DIR is not empty at start: %s" % sorted(os.listdir(runtime)))
    try:
        with open("/proc/self/net/dev") as f:
            ifaces = sorted(line.split(":")[0].strip() for line in f.readlines()[2:])
        if ifaces != ["lo"]:
            problems.append("network namespace not private: %s" % ifaces)
    except OSError as e:
        problems.append("cannot read /proc/self/net/dev: %s" % e)
    # The gate itself runs under a coding agent on the host; inside the sandbox's process namespace no agent may be
    # among this process's ancestors, or the "plain shell" caller would be identified as that agent.
    pid = os.getpid()
    chain = []
    while pid > 0:
        try:
            comm, ppid = comm_and_ppid(pid)
        except OSError:
            break
        chain.append(comm)
        if comm in LAUNCHERS:
            problems.append("a process named %s is among the gate's ancestors" % comm)
        pid = ppid
    if len(chain) > 6:
        problems.append("ancestry too long for a private process namespace: %s" % chain)
    return problems


# --- the table ---------------------------------------------------------------------------------------------------

def text_of(el):
    t = el.find("text")
    if t is not None:
        return "".join(p.text or "" for p in t.findall("part"))
    return el.get("text")


def load_m1(path):
    root = ET.parse(path).getroot()
    reasons = {r.get("code"): r.get("rule") for r in root.find("reasons")}
    cases = {}
    for c in root.findall("case"):
        d = dict(c.attrib)
        d["text"] = text_of(c)
        cases[d["id"]] = d
    return reasons, cases


def rule_number(rule):
    m = re.match(r"R(\d+)", rule or "")
    return int(m.group(1)) if m else 99


INSTANCE_M1_CONFIG = {
    "tests/policy/fixtures/main.xml": "main",
    "tests/policy/fixtures/alt.xml": "alt",
    "tests/policy/fixtures/invalid.xml": "invalid",
    "": "missing",
}


def sandbox_facts(call, window, stopped):
    """The facts of a call as the service will see them in the sandbox, in M1's terms."""
    cmd = call.get("command")
    f = {"client": CALLER_CLIENT[call.get("caller")], "action": cmd, "stopped": stopped}
    if cmd == "launch":
        f["class"] = call.get("app")
    elif window is not None:
        f["class"], f["title"], f["monitor"] = window["class"], window["title"], window["monitor"]
        f["agent"] = PROCESS_AGENT[window.get("process", "")]
    focused = window is not None and call.get("active") == window["id"]
    f["focused"] = focused
    f["password"] = "unknown" if cmd in ("type", "key") and focused else "false"
    f["focus-moved"] = False
    if cmd == "type":
        f["text"] = text_of(call)
    elif cmd == "key":
        f["text"] = call.get("key")
    return f


def m1_mismatches(m1, facts, reasons, config_name):
    """Facts that differ from the M1 case, and the ones among them that could change the result."""
    diffs, relevant = [], []

    def differ(name, ours, theirs):   # a fact every rule may read: any difference can change the result
        diffs.append("%s: M1 %r, here %r" % (name, theirs, ours))
        relevant.append(name)

    if m1.get("config") != config_name:
        differ("config", config_name, m1.get("config"))
    if m1.get("client") != facts["client"]:
        differ("client", facts["client"], m1.get("client"))
    if m1.get("action") != facts["action"]:
        differ("action", facts["action"], m1.get("action"))
    # resume has no target; a refused windows listing (R1-R4) has none either (its windows are checked as <item>s).
    if facts["action"] not in ("resume", "stop", "segfault") and not (facts["action"] == "windows"
                                                                      and "class" not in facts):
        if m1.get("class") is not None and m1.get("class") != facts.get("class"):
            differ("class", facts.get("class"), m1.get("class"))
        if facts["action"] != "launch":
            if m1.get("title") is not None and m1.get("title") != facts.get("title"):
                differ("title", facts.get("title"), m1.get("title"))
            if m1.get("monitor", "DP-2") != facts.get("monitor"):
                differ("monitor", facts.get("monitor"), m1.get("monitor", "DP-2"))
            if m1.get("agent", "") != facts.get("agent", ""):
                differ("agent", facts.get("agent"), m1.get("agent", ""))
    if (m1.get("stopped", "false") == "true") != facts["stopped"]:
        differ("stopped", facts["stopped"], m1.get("stopped", "false"))
    if facts["action"] in ("type", "key") and m1.get("text") is not None and m1.get("text") != facts.get("text"):
        differ("text", facts.get("text"), m1.get("text"))
    # Facts only R9 (password) and R11 (focused, focus-moved) read, and only for type and key: a difference is
    # harmless when the M1 case is decided by an earlier rule.
    if facts["action"] in ("type", "key"):
        decided_by = rule_number(reasons.get(m1.get("reason")))
        for name, ours, theirs, reading in (
                ("password", facts["password"], m1.get("password", "false"), 9),
                ("focused", "true" if facts["focused"] else "false", m1.get("focused", "false"), 11),
                ("focus-moved", "false", m1.get("focus-moved", "false"), 11)):
            if ours != theirs:
                diffs.append("%s: M1 %r, here %r" % (name, theirs, ours))
                if decided_by >= reading:
                    relevant.append(name)
    return diffs, relevant


def check_table(cases_root, reasons, m1_cases, windows):
    """Structural checks of tests/cli/cases.xml against the M1 table. Returns nothing; reports PASS/FAIL."""
    problems = []
    mapped = set()
    seen_ids = set()
    coverage = {"claude": set(), "pi": set()}
    for inst in cases_root.findall("instance"):
        config_name = INSTANCE_M1_CONFIG.get(inst.get("config"))
        stopped = False
        for step in inst:
            ident = step.get("id")
            if ident in seen_ids:
                problems.append("%s: duplicate id" % ident)
            seen_ids.add(ident)
            if step.tag != "call":
                continue
            cmd, caller, kind = step.get("command"), step.get("caller"), step.get("kind")
            if cmd not in SUBCOMMANDS:
                problems.append("%s: unknown subcommand %s" % (ident, cmd))
            if caller not in CALLER_CLIENT:
                problems.append("%s: unknown caller %s" % (ident, caller))
                continue
            if caller in coverage:
                coverage[caller].add(cmd)
            window = windows.get(step.get("window")) if step.get("window") else None
            if cmd in WINDOWED and window is None:
                problems.append("%s: %s needs a desk window" % (ident, cmd))
            if step.get("active") and step.get("active") not in windows:
                problems.append("%s: active window %s not on the desk" % (ident, step.get("active")))
            if step.get("expect") not in ("allow", "refused", "list", "ok", "help"):
                problems.append("%s: bad expect" % ident)
            if step.get("expect") == "refused" and not step.get("reason"):
                problems.append("%s: refused without a reason" % ident)
            if step.get("expect") == "allow" and not step.get("rule"):
                problems.append("%s: allow without a rule" % ident)
            if not (step.findtext("why") or "").strip():
                problems.append("%s: no <why>" % ident)
            facts = sandbox_facts(step, window, stopped)
            m1id = step.get("m1")
            if kind in ("m1", "derived"):
                m1 = m1_cases.get(m1id)
                if m1 is None:
                    problems.append("%s: M1 case %s does not exist" % (ident, m1id))
                elif config_name is None:
                    problems.append("%s: instance %s has no M1 fixture" % (ident, inst.get("id")))
                else:
                    mapped.add(m1id)
                    diffs, relevant = m1_mismatches(m1, facts, reasons, config_name)
                    if kind == "m1":
                        if relevant:
                            problems.append("%s: facts differ from %s where they decide: %s" % (ident, m1id, "; ".join(diffs)))
                        exp, reason, rule = m1.get("expect"), m1.get("reason"), m1.get("rule")
                        if exp == "allow":
                            want = "ok" if cmd == "resume" else "allow"
                            if step.get("expect") != want or (want == "allow" and step.get("rule") != rule):
                                problems.append("%s: expected result differs from %s (allow %s)" % (ident, m1id, rule))
                        elif step.get("expect") != "refused" or step.get("reason") != reason \
                                or (rule and step.get("rule") != rule):
                            problems.append("%s: expected result differs from %s (deny %s)" % (ident, m1id, reason))
                    else:
                        if not (step.findtext("differs") or "").strip():
                            problems.append("%s: derived without <differs>" % ident)
                        if not relevant:
                            problems.append("%s: derived, but no deciding fact differs from %s: make it kind m1" % (ident, m1id))
            elif kind == "extra":
                if m1id:
                    problems.append("%s: extra with an m1 id" % ident)
            else:
                problems.append("%s: bad kind %s" % (ident, kind))
            for item in step.findall("item"):
                w = windows.get(item.get("window"))
                if w is None:
                    problems.append("%s: item window %s not on the desk" % (ident, item.get("window")))
                    continue
                if item.get("m1"):
                    m1 = m1_cases.get(item.get("m1"))
                    if m1 is None:
                        problems.append("%s: M1 case %s does not exist" % (ident, item.get("m1")))
                        continue
                    mapped.add(item.get("m1"))
                    ifacts = sandbox_facts(step, w, stopped)
                    diffs, relevant = m1_mismatches(m1, ifacts, reasons, config_name)
                    if relevant:
                        problems.append("%s/%s: facts differ from %s: %s" % (ident, w["id"], item.get("m1"), "; ".join(diffs)))
                    if (item.get("listed") == "true") != (m1.get("expect") == "allow"):
                        problems.append("%s/%s: listed differs from %s" % (ident, w["id"], item.get("m1")))
            if cmd in ("stop", "segfault") and step.get("expect") == "ok":
                stopped = True
            if cmd == "resume" and step.get("expect") == "ok":
                stopped = False
    excluded = {e.get("m1") for e in cases_root.findall("excluded")}
    want = {cid for cid, c in m1_cases.items() if c.get("client") in ("claude-code", "pi")}
    missing = sorted(want - mapped - excluded, key=lambda s: int(s[1:]))
    report(not problems, "table: every call well formed and agreeing with the M1 table",
           "; ".join(problems[:12]) + (" ... (%d problems)" % len(problems) if len(problems) > 12 else ""))
    report(not missing, "table: every M1 case for claude-code or pi is mapped or excluded with a reason",
           "unmapped: " + " ".join(missing) if missing else "%d mapped, %d excluded" % (len(want & mapped), len(excluded)))
    for caller in ("claude", "pi"):
        lacking = [c for c in SUBCOMMANDS if c not in coverage[caller]]
        report(not lacking, "table: every subcommand called as %s" % caller,
               "missing: " + " ".join(lacking) if lacking else "")


# --- fake Hyprland -----------------------------------------------------------------------------------------------

class FakeHyprland:
    def __init__(self, path, monitors, windows):
        self.path = path
        self.monitors = monitors
        self.windows = windows
        self.active = None
        self.dispatches = 0
        self.lock = threading.Lock()
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(path)
        os.chmod(path, 0o600)
        self.sock.listen(32)
        threading.Thread(target=self.loop, daemon=True).start()

    def client_json(self, w, index):
        mon = self.monitors[w["monitor"]]
        x = mon["x"] + (60 if w["monitor"] == "DP-2" else 40)
        y = mon["y"] + 50
        return {"address": w["address"], "mapped": True, "hidden": False, "at": [x, y], "size": [800, 600],
                "workspace": {"id": mon["workspace"], "name": str(mon["workspace"])}, "floating": False,
                "pseudo": False, "monitor": mon["id"], "class": w["class"], "title": w["title"],
                "initialClass": w["class"], "initialTitle": w["title"], "pid": w.get("pid", 0),
                "xwayland": False, "pinned": False, "fullscreen": 0, "fullscreenClient": 0, "grouped": [],
                "tags": [], "swallowing": "0x0", "focusHistoryID": index}

    def answer(self, req):
        req = req.strip()
        if req.startswith("/"):
            req = req[1:]
        if req.startswith("dispatch") or "dispatch " in req:
            with self.lock:
                self.dispatches += 1
            return b"error: the CLI gate's fake Hyprland does not dispatch"
        if req == "j/monitors":
            return json.dumps([{"id": m["id"], "name": m["name"], "x": m["x"], "y": m["y"], "width": m["width"],
                                "height": m["height"], "scale": m["scale"],
                                "activeWorkspace": {"id": m["workspace"], "name": str(m["workspace"])},
                                "focused": m["name"] == "DP-2"} for m in self.monitors.values()]).encode()
        if req == "j/clients":
            return json.dumps([self.client_json(w, i) for i, w in enumerate(self.windows.values())]).encode()
        if req == "j/activewindow":
            with self.lock:
                active = self.active
            if active is None:
                return b"{}"
            return json.dumps(self.client_json(self.windows[active], 0)).encode()
        if req == "j/cursorpos":
            return b'{"x": 0, "y": 0}'
        return b"unknown request"

    def loop(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self.serve, args=(conn,), daemon=True).start()

    def serve(self, conn):
        try:
            conn.settimeout(2.0)
            data = conn.recv(65536)
            conn.sendall(self.answer(data.decode("utf-8", "replace")))
        except OSError:
            pass
        finally:
            conn.close()


# --- the service and the calls -----------------------------------------------------------------------------------

class Instance:
    def __init__(self, ident, home, agentd, runtime, outdir):
        self.ident, self.home, self.agentd, self.runtime, self.outdir = ident, home, agentd, runtime, outdir
        self.proc = None
        self.socket = os.path.join(runtime, "riley-desktop-agent", "agentd.sock")
        self.log = os.path.join(home, ".local", "state", "riley-desktop-agent", "log.xml")
        self.bar = os.path.join(runtime, "riley-desktop-agent", "state.json")
        self.runtime_dir = os.path.join(runtime, "riley-desktop-agent")
        self.state_dir = os.path.join(home, ".local", "state", "riley-desktop-agent")
        self.stop_copy = os.path.join(self.state_dir, "stopped")   # the persisted stop copy (docs/files.md)
        self.start_offset = 0
        self.env = {
            "PATH": "/usr/bin:/bin", "HOME": home, "LANG": "C.UTF-8",
            "XDG_RUNTIME_DIR": runtime, "XDG_CONFIG_HOME": os.path.join(home, ".config"),
            "XDG_STATE_HOME": os.path.join(home, ".local", "state"),
            "HYPRLAND_INSTANCE_SIGNATURE": FAKE_SIGNATURE,
        }
        self.starts = 0

    def start(self):
        self.starts += 1
        self.start_offset = self.log_size()
        out = open(os.path.join(self.outdir, "%s-agentd-%d.log" % (self.ident, self.starts)), "wb")
        self.proc = subprocess.Popen([self.agentd], env=self.env, cwd=self.home, stdin=subprocess.DEVNULL,
                                     stdout=out, stderr=subprocess.STDOUT)
        out.close()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                return "riley-agentd exited with %s (see %s-agentd-%d.log)" % (self.proc.returncode, self.ident,
                                                                            self.starts)
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(self.socket)
                return None
            except OSError:
                time.sleep(0.05)
            finally:
                s.close()
        return "riley-agentd did not listen on %s within 10 s" % self.socket

    def kill(self, sig):
        if self.proc is None or self.proc.poll() is not None:
            return
        os.kill(self.proc.pid, sig)
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.kill(self.proc.pid, signal.SIGKILL)
            self.proc.wait(timeout=5)

    def log_size(self):
        try:
            return os.path.getsize(self.log)
        except OSError:
            return 0

    def log_since(self, offset):
        try:
            with open(self.log, "rb") as f:
                f.seek(offset)
                return [l for l in f.read().decode("utf-8", "replace").splitlines() if l.strip()]
        except OSError:
            return []


def call_argv(call, windows, cli, bindir, shots):
    cmd = call.get("command")
    args = [cli, cmd]
    w = windows.get(call.get("window")) if call.get("window") else None
    if cmd in WINDOWED:
        args += ["--window", w["address"]]
    if cmd == "see":
        args += ["--png", os.path.join(shots, call.get("id") + ".png")]
    elif cmd in ("point", "click"):
        args += ["10", "10"]
    elif cmd == "type":
        args += [text_of(call)]
    elif cmd == "key":
        args += [call.get("key")]
    elif cmd == "launch":
        args += [call.get("app")]
    caller = call.get("caller")
    if caller == "shell":
        return ["/usr/bin/timeout", str(CALL_TIMEOUT_S), "/bin/bash", "-c", '"$@"; exit $?', "bash"] + args
    return [os.path.join(bindir, caller), str(CALL_TIMEOUT_S)] + args


def plain_problems(raw):
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        return ["output is not UTF-8"], ""
    bad = [c for c in text if (ord(c) < 0x20 and c not in "\n\t") or ord(c) == 0x7f]
    problems = []
    if bad:
        problems.append("control characters in the output (%r)" % "".join(sorted(set(bad)))[:20])
    if not text.endswith("\n"):
        problems.append("output does not end with a newline")
    if text.lstrip().startswith(("{", "[", "<")):
        problems.append("output is not plain text")
    return problems, text


def parse_log_line(line):
    try:
        el = ET.fromstring(line)
    except ET.ParseError:
        return None
    return el


def run_call(call, inst, fake, windows, cli, bindir, shots, transcript):
    ident = call.get("id")
    cmd, caller, expect = call.get("command"), call.get("caller"), call.get("expect")
    with fake.lock:
        fake.active = call.get("active")
        dispatches_before = fake.dispatches
    log_before = inst.log_size()
    argv = call_argv(call, windows, cli, bindir, shots)
    try:
        r = subprocess.run(argv, env=inst.env, cwd=inst.home, stdin=subprocess.DEVNULL, capture_output=True,
                           timeout=CALL_TIMEOUT_S + 10)
        rc, out, err = r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        rc, out, err = None, b"", b""
    with fake.lock:
        dispatched = fake.dispatches - dispatches_before
        fake.active = None
    new_log = inst.log_since(log_before)
    transcript.write("== %s %s %s rc=%s\n%s%s-- log:\n%s\n" % (
        ident, caller, " ".join(argv[argv.index(cli) + 1:]) if cli in argv else "", rc,
        out.decode("utf-8", "replace"), ("-- stderr:\n" + err.decode("utf-8", "replace")) if err else "",
        "\n".join(new_log)))
    transcript.flush()

    problems = []
    if rc is None:
        report(False, ident, "%s %s did not finish within %d s" % (caller, cmd, CALL_TIMEOUT_S + 10))
        return
    plain, text = plain_problems(out)
    problems += plain
    lines = text.splitlines()
    first = lines[0] if lines else ""
    desc = "%s %s%s" % (caller, cmd, (" " + call.get("window")) if call.get("window") else "")

    if expect == "help":
        if rc != 0:
            problems.append("exit %s, not 0" % rc)
        if len(lines) >= 60:
            problems.append("%d lines (must be under 60)" % len(lines))
        unnamed = [s for s in SUBCOMMANDS if not re.search(r"(?<![\w-])%s(?![\w-])" % re.escape(s), text)]
        if unnamed:
            problems.append("does not name: " + " ".join(unnamed))
        if "\x1b" in text:
            problems.append("terminal escape codes")
        if err:
            problems.append("wrote to stderr")
        if new_log:
            problems.append("help reached the service (%d log lines)" % len(new_log))
        report(not problems, ident, "; ".join(problems) if problems else "%s: %d lines, every subcommand named" % (desc, len(lines)))
        return

    m = RESULT_RE.match(first)
    if not m:
        problems.append("first line %r is no result (ok, refused <reason>, stopped, failed <what>)" % first[:120])
    else:
        word = m.group(1).split(" ")[0]
        if rc != EXIT_FOR[word]:
            problems.append("exit %s does not match %r (want %d)" % (rc, word, EXIT_FOR[word]))
    if expect != "list" and len(lines) != 1:
        problems.append("%d output lines, one result expected" % len(lines))
    if len(new_log) != 1:
        problems.append("%d new log lines, one expected" % len(new_log))
    rec = parse_log_line(new_log[0]) if len(new_log) == 1 else None
    if len(new_log) == 1 and rec is None:
        problems.append("log line is not one XML element")

    def want_attr(name, value):
        if rec is not None and rec.get(name) != value:
            problems.append("log %s=%r, want %r" % (name, rec.get(name), value))

    if rec is not None:
        client = rec.get("client")
        want_client = CALLER_CLIENT[caller]
        if (caller == "shell" and client not in ("", "unknown")) or (caller != "shell" and client != want_client):
            problems.append("log client=%r, want %r" % (client, want_client or "'' or 'unknown'"))
        if rec.get("action") != cmd:
            problems.append("log action=%r, want %r" % (rec.get("action"), cmd))
        if cmd == "type":
            typed = text_of(call)
            if typed and len(typed) >= 3 and typed in new_log[0]:
                problems.append("the typed text is in the log")

    if expect == "allow":
        if not first.startswith("failed ") or rc != 4:
            problems.append("first line %r exit %s: an allowed action must fail harmlessly in the sandbox "
                            "(failed, exit 4)" % (first[:100], rc))
        if first.startswith(("failed no-window", "failed no-desktop", "failed busy", "failed no-answer",
                             "failed no-service", "failed bad-")):
            problems.append("failed before the decision")
        if rec is not None:
            if rec.tag != "request":
                problems.append("log line is <%s>, want <request>" % rec.tag)
            want_attr("decision", "allow")
            want_attr("reason", "allowed")
            want_attr("rule", call.get("rule"))
            want_attr("result", "failed")
    elif expect == "refused":
        reason = call.get("reason")
        if not (first == "refused " + reason or first.startswith("refused " + reason + " ")
                or first.startswith("refused " + reason + ":")):
            problems.append("first line %r, want 'refused %s'" % (first[:100], reason))
        if rc != 1:
            problems.append("exit %s, want 1" % rc)
        if dispatched:
            problems.append("%d Hyprland dispatches after a refusal" % dispatched)
        if rec is not None:
            if cmd == "resume":
                if rec.tag != "control":
                    problems.append("log line is <%s>, want <control>" % rec.tag)
            else:
                if rec.tag != "request":
                    problems.append("log line is <%s>, want <request>" % rec.tag)
                want_attr("decision", "deny")
                want_attr("reason", reason)
                if call.get("rule"):
                    want_attr("rule", call.get("rule"))
            want_attr("result", "refused")
    elif expect == "ok":
        if not (first == "ok" or first.startswith("ok ")) or rc != 0:
            problems.append("first line %r exit %s, want ok and 0" % (first[:100], rc))
        if dispatched:
            problems.append("%d Hyprland dispatches" % dispatched)
        if rec is not None:
            if rec.tag != "control":
                problems.append("log line is <%s>, want <control>" % rec.tag)
            want_attr("result", "done")
    elif expect == "list":
        mm = re.match(r"^ok (\d+) windows?$", first)
        if not mm or rc != 0:
            problems.append("first line %r exit %s, want 'ok N windows' and 0" % (first[:100], rc))
        else:
            body = lines[1:]
            if int(mm.group(1)) != len(body):
                problems.append("says %s windows, lists %d" % (mm.group(1), len(body)))
            listed = {l.split(" ")[0] for l in body}
            for item in call.findall("item"):
                w = windows[item.get("window")]
                want = item.get("listed") == "true"
                if (w["address"] in listed) != want:
                    problems.append("%s %s %s" % (item.get("window"), w["class"],
                                                  "missing from the listing" if want else "listed, must not be"))
        if dispatched:
            problems.append("%d Hyprland dispatches" % dispatched)
        if rec is not None:
            want_attr("decision", "allow")
            want_attr("rule", "per-window")
            want_attr("result", "done")
    report(not problems, ident, ("%s: " % desc) + ("; ".join(problems) if problems else first[:100]))


def read_bar(path):
    try:
        with open(path) as f:
            return json.loads(f.read()).get("state")
    except (OSError, ValueError, AttributeError):
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--build", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--plant", default="")
    ap.add_argument("--part", choices=("main", "sandbox"), default="main")
    a = ap.parse_args()
    if a.part == "sandbox":
        return sandbox_part(a)

    runtime = os.environ.get("XDG_RUNTIME_DIR", "")
    problems = sandbox_problems(runtime)
    report(not problems, "sandbox: no uinput, no Wayland/X/bus, no real Hyprland, private namespaces",
           "; ".join(problems))
    if problems:
        print("FAIL (the sandbox is not safe; nothing was started)")
        return 1

    reasons, m1_cases = load_m1(os.path.join(a.repo, "tests/policy/cases.xml"))
    root = ET.parse(os.path.join(a.repo, "tests/cli/cases.xml")).getroot()
    desk = root.find("desk")
    monitors = {}
    for i, m in enumerate(desk.findall("monitor")):
        monitors[m.get("name")] = {"id": int(m.get("id")), "name": m.get("name"), "x": int(m.get("x")),
                                   "y": int(m.get("y")), "width": int(m.get("width")),
                                   "height": int(m.get("height")), "scale": float(m.get("scale")),
                                   "workspace": i + 1}
    windows = {}
    for w in desk.findall("window"):
        windows[w.get("id")] = dict(w.attrib)
    check_table(root, reasons, m1_cases, windows)

    agentd = os.path.join(a.build, "src/agentd/riley-agentd")
    cli = os.path.join(a.build, "src/cli/riley-agent")
    for exe in (agentd, cli):
        report(os.access(exe, os.X_OK), "binary %s" % os.path.basename(exe), "" if os.access(exe, os.X_OK) else "missing")
    if not (os.access(agentd, os.X_OK) and os.access(cli, os.X_OK)):
        print("FAIL (%d checks failed)" % FAILS)
        return 1

    work = "/tmp/riley-cli-gate"
    bindir = os.path.join(work, "bin")
    os.makedirs(bindir)
    for name in LAUNCHERS:   # the service sees comm claude / pi / codex among the caller's ancestors
        shutil.copy2("/usr/bin/timeout", os.path.join(bindir, name))
    shots = os.path.join(runtime, "cli-gate-shots")
    os.mkdir(shots, 0o700)
    os.chmod(shots, 0o700)
    hyprdir = os.path.join(runtime, "hypr", FAKE_SIGNATURE)
    os.makedirs(hyprdir, 0o700)

    holders = []
    for w in windows.values():
        if w.get("process"):
            p = subprocess.Popen([os.path.join(bindir, w["process"]), "3600", "/usr/bin/sleep", "3600"],
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            holders.append(p)
            w["pid"] = p.pid
    fake = FakeHyprland(os.path.join(hyprdir, ".socket.sock"), monitors, windows)
    transcript = open(os.path.join(a.out, "calls.txt"), "w")

    try:
        for inst_el in root.findall("instance"):
            ident = inst_el.get("id")
            home = os.path.join(work, "home-" + ident)
            os.makedirs(home, 0o700)
            if inst_el.get("config"):
                cdir = os.path.join(home, ".config", "riley-desktop-agent")
                os.makedirs(cdir, 0o700)
                shutil.copyfile(os.path.join(a.repo, inst_el.get("config")), os.path.join(cdir, "config.xml"))
            inst = Instance(ident, home, agentd, runtime, a.out)
            problem = inst.start()
            report(problem is None, "%s: service starts" % ident, problem or "")
            for step in inst_el:
                sid = step.get("id")
                if inst.proc is None or inst.proc.poll() is not None:
                    report(False, sid, "the service is not running (exit %s)" %
                           (inst.proc.returncode if inst.proc else None))
                    continue
                if step.tag == "call":
                    run_call(step, inst, fake, windows, cli, bindir, shots, transcript)
                elif step.tag == "restart":
                    inst.kill(signal.SIGKILL)
                    try:
                        os.unlink(inst.bar)
                    except OSError:
                        pass
                    wiped = ""
                    if step.get("wipe") == "true":   # check 7: only log.xml and hash-secret are left
                        had_copy = os.path.lexists(inst.stop_copy)
                        try:
                            os.unlink(inst.stop_copy)
                        except FileNotFoundError:
                            pass
                        had_runtime = sorted(os.listdir(inst.runtime_dir)) if os.path.isdir(inst.runtime_dir) else []
                        shutil.rmtree(inst.runtime_dir, ignore_errors=True)
                        gone = not os.path.lexists(inst.stop_copy) and not os.path.lexists(inst.runtime_dir)
                        wiped = "; deleted the stop copy (%s) and the runtime folder (%s)%s" % (
                            "was there" if had_copy else "was not there", " ".join(had_runtime) or "empty",
                            "" if gone else ": NOT GONE")
                        if not gone:
                            report(False, sid, "could not delete the stop copy or the runtime folder")
                            continue
                    problem = inst.start()
                    report(problem is None, sid, "service killed by PID (SIGKILL) and started again with the same "
                           "state folder" + wiped + (": " + problem if problem else ""))
                elif step.tag == "bar":
                    state = read_bar(inst.bar)
                    report(state == step.get("state"), sid, "bar state %r, want %r" % (state, step.get("state")))
                elif step.tag == "persisted":
                    want = step.get("present") == "true"
                    problems = []
                    if os.path.lexists(inst.stop_copy) != want:
                        problems.append("stop copy %s" % ("missing" if want else "still there"))
                    elif want:
                        st = os.lstat(inst.stop_copy)
                        if not stat.S_ISREG(st.st_mode) or stat.S_IMODE(st.st_mode) != 0o600:
                            problems.append("stop copy is not a 0600 file (mode %o)" % st.st_mode)
                        dmode = stat.S_IMODE(os.stat(inst.state_dir).st_mode)
                        if dmode != 0o700:
                            problems.append("state folder mode %o, want 700" % dmode)
                    report(not problems, sid, "; ".join(problems) if problems else
                           "stop copy %s" % ("there, 0600 in a 0700 folder" if want else "not there"))
                elif step.tag == "startlog":
                    lines = inst.log_since(inst.start_offset)
                    els = [parse_log_line(l) for l in lines]
                    word = step.get("contains")
                    problems = []
                    if not lines:
                        problems.append("no log.xml line written at start")
                    if any(e is None for e in els):
                        problems.append("a start-up log line is not one XML element")
                    if not any(e is not None and any(word in v for v in e.attrib.values()) for e in els):
                        problems.append("no start-up log line says why (no attribute value containing %r)" % word)
                    report(not problems, sid, "; ".join(problems) if problems else
                           "%d start-up log line(s): %s" % (len(lines), lines[-1][:160]))
            inst.kill(signal.SIGTERM)
            if inst.proc is not None and inst.proc.returncode not in (0, -signal.SIGTERM):
                report(False, "%s: service exits cleanly" % ident, "exit %s" % inst.proc.returncode)
    finally:
        transcript.close()
        for p in holders:
            if p.poll() is None:
                os.kill(p.pid, signal.SIGTERM)
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.kill(p.pid, signal.SIGKILL)
                    p.wait(timeout=5)
    left = os.listdir(shots)
    report(not left, "no picture was written", "found: %s" % left if left else "")

    if a.plant:
        targeted = [c.get("id") for c in root.iter() if c.get("targets") == a.plant]
        missed = [t for t in targeted if t not in FAILED_IDS]
        if targeted and not missed:
            print("PLANTED FAULT %s CAUGHT by: %s" % (a.plant, " ".join(targeted)))
        else:
            print("PLANTED FAULT %s NOT CAUGHT (targeted calls still passing: %s)" % (a.plant, " ".join(missed) or "none targeted"))
    return finish("main")


def finish(part):
    if FAILS == 0:
        print("PART %s: PASS" % part)
        return 0
    print("PART %s: FAIL (%d checks failed)" % (part, FAILS))
    return 1


SANDBOX_SECTION = "What the permissions can and cannot stop"


def doc_section(path):
    """The body of the Markdown section titled SANDBOX_SECTION, or None."""
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        return None
    for i, line in enumerate(lines):
        m = re.match(r"^(#+)\s+(.*?)\s*$", line)
        if m and m.group(2) == SANDBOX_SECTION:
            level, body = len(m.group(1)), []
            for nxt in lines[i + 1:]:
                h = re.match(r"^(#+)\s", nxt)
                if h and len(h.group(1)) <= level:
                    break
                body.append(nxt)
            return "\n".join(body)
    return None


def parse_probe(text):
    values, agent_lines, agent_rc = {}, [], None
    for line in text.splitlines():
        if line.startswith("probe "):
            parts = line.split(" ", 2)
            if len(parts) == 3:
                values[parts[1]] = parts[2]
        elif line.startswith("agent-exit "):
            agent_rc = int(line.split(" ", 1)[1]) if line.split(" ", 1)[1].isdigit() else None
        elif line.startswith("agent-out "):
            agent_lines.append(line[len("agent-out "):])
    return values, agent_lines, agent_rc


def file_digest(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return None


def sandbox_part(a):
    """Checks 8 and 9: the documented sandbox wrapper, on a decoy host; the docs section."""
    runtime = os.environ.get("XDG_RUNTIME_DIR", "")
    problems = sandbox_problems(runtime, decoy_uinput=True)
    report(not problems, "sandbox: decoy uinput is /dev/null, no real Wayland/X/bus/Hyprland, private namespaces",
           "; ".join(problems))
    if problems:
        print("PART sandbox: FAIL (the sandbox is not safe; nothing was started)")
        return 1

    root = ET.parse(os.path.join(a.repo, "tests/cli/cases.xml")).getroot()
    ex = root.find("sandbox-example")

    # check 9: the docs
    for doc in ex.findall("doc"):
        body = doc_section(os.path.join(a.repo, doc.get("file")))
        if body is None:
            report(False, doc.get("id"), "%s has no section titled %r" % (doc.get("file"), SANDBOX_SECTION))
            continue
        missing = [w for w in doc.get("names").split() if w not in body]
        report(not missing, doc.get("id"), "%s section %r %s" % (
            doc.get("file"), SANDBOX_SECTION, "does not name: " + " ".join(missing) if missing else "names all of: " +
            doc.get("names")))

    # check 8: the wrapper on the decoy host
    wrapper = os.path.join(a.repo, "tools", "riley-sandbox")
    agentd = os.path.join(a.build, "src/agentd/riley-agentd")
    cli = os.path.join(a.build, "src/cli/riley-agent")
    ok = True
    for exe in (wrapper, agentd, cli):
        good = os.path.isfile(exe) and os.access(exe, os.X_OK)
        report(good, "k800: %s" % os.path.relpath(exe, a.repo) if exe == wrapper else "k800: binary %s" %
               os.path.basename(exe), "" if good else "missing or not executable")
        ok = ok and good
    if not ok:
        return finish("sandbox")

    desk = root.find("desk")
    monitors = {}
    for i, m in enumerate(desk.findall("monitor")):
        monitors[m.get("name")] = {"id": int(m.get("id")), "name": m.get("name"), "x": int(m.get("x")),
                                   "y": int(m.get("y")), "width": int(m.get("width")),
                                   "height": int(m.get("height")), "scale": float(m.get("scale")),
                                   "workspace": i + 1}
    windows = {w.get("id"): dict(w.attrib) for w in desk.findall("window")}
    listing = next(c for c in root.iter("call") if c.get("id") == ex.get("listing"))

    work = "/tmp/riley-cli-gate"
    bindir = os.path.join(work, "bin")
    os.makedirs(bindir)
    for name in LAUNCHERS:
        shutil.copy2("/usr/bin/timeout", os.path.join(bindir, name))
    home = os.path.join(work, "home-sbx")
    project = os.path.join(home, "git", "project")
    os.makedirs(project, 0o700)
    cdir = os.path.join(home, ".config", "riley-desktop-agent")
    os.makedirs(cdir, 0o700)
    config = os.path.join(cdir, "config.xml")
    shutil.copyfile(os.path.join(a.repo, ex.get("config")), config)
    localbin = os.path.join(home, ".local", "bin")
    os.makedirs(localbin)
    agent = os.path.join(localbin, "riley-agent")
    shutil.copy2(cli, agent)
    probe = os.path.join(project, "sandbox-probe.sh")
    shutil.copy2(os.path.join(a.repo, "tests/cli/sandbox-probe.sh"), probe)
    os.chmod(probe, 0o755)
    marker = os.path.join(home, ".riley-gate-home-marker")
    with open(marker, "w") as f:
        f.write("fake marker for the CLI gate\n")

    # The decoys (all inside this sandbox's private tmpfs; none is served).
    hyprdir = os.path.join(runtime, "hypr", FAKE_SIGNATURE)
    os.makedirs(hyprdir, 0o700)
    decoys = []
    for path in (os.path.join(runtime, "wayland-0"), "/tmp/.X11-unix/X0"):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.bind(path)
        decoys.append(s)

    holders = []
    for w in windows.values():
        if w.get("process"):
            p = subprocess.Popen([os.path.join(bindir, w["process"]), "3600", "/usr/bin/sleep", "3600"],
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            holders.append(p)
            w["pid"] = p.pid
    fake = FakeHyprland(os.path.join(hyprdir, ".socket.sock"), monitors, windows)
    inst = Instance("sbx", home, agentd, runtime, a.out)
    caller = ex.get("caller")
    transcript = open(os.path.join(a.out, "sandbox-probe.txt"), "w")
    try:
        problem = inst.start()
        report(problem is None, "k800: service starts", problem or "")
        if problem is None:
            env = dict(inst.env)
            env.update({"WAYLAND_DISPLAY": "wayland-0", "DISPLAY": ":0", "PATH": localbin + ":/usr/bin:/bin"})
            probe_args = [probe, runtime, config, inst.state_dir, agent, marker]
            results = {}
            for mode in ("wrapped", "bare"):
                config_before = file_digest(config)
                log_before = inst.log_size()
                inner = ([wrapper] if mode == "wrapped" else []) + ["/bin/sh"] + probe_args
                argv = [os.path.join(bindir, caller), str(CALL_TIMEOUT_S)] + inner
                try:
                    r = subprocess.run(argv, env=env, cwd=project, stdin=subprocess.DEVNULL, capture_output=True,
                                       timeout=CALL_TIMEOUT_S + 10)
                    rc, out, err = r.returncode, r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")
                except subprocess.TimeoutExpired:
                    rc, out, err = None, "", ""
                transcript.write("== %s rc=%s\n%s-- stderr:\n%s\n" % (mode, rc, out, err))
                values, agent_lines, agent_rc = parse_probe(out)
                # The service itself may add log.xml on the call; what counts is the probe's own file.
                planted = os.path.lexists(os.path.join(inst.state_dir, "probe-was-here"))
                results[mode] = {
                    "rc": rc, "values": values, "agent_lines": agent_lines, "agent_rc": agent_rc,
                    "log": inst.log_since(log_before),
                    "config": "unchanged" if file_digest(config) == config_before else "changed",
                    "state": "changed" if planted else "unchanged",
                }
                # Undo what a bare probe could change, so the next run starts the same.
                if file_digest(config) != config_before and config_before is not None:
                    with open(config, "wb") as f:
                        f.write(config_before)
                for name in ("probe-was-here",):
                    try:
                        os.unlink(os.path.join(inst.state_dir, name))
                    except OSError:
                        pass
                try:
                    os.unlink(os.path.join(project, "probe-project-write"))
                except OSError:
                    pass
            for mode in ("wrapped", "bare"):
                if results[mode]["rc"] is None:
                    report(False, "k800: probe %s" % mode, "did not finish within %d s" % (CALL_TIMEOUT_S + 10))
                elif results[mode]["values"].get("cwd") != project:
                    report(False, "k800: probe %s" % mode, "ran in %r, want the project %s" % (
                        results[mode]["values"].get("cwd"), project))
            for pr in ex.findall("probe"):
                problems = []
                for mode in ("wrapped", "bare"):
                    got = results[mode]["values"].get(pr.get("name"))
                    if got != pr.get(mode):
                        problems.append("%s: %r, want %r" % (mode, got, pr.get(mode)))
                report(not problems, pr.get("id"), "%s: %s" % (pr.get("name"), "; ".join(problems) if problems else
                                                              "wrapped %s, bare %s" % (pr.get("wrapped"), pr.get("bare"))))
            for un in ex.findall("untouched"):
                problems = []
                for mode in ("wrapped", "bare"):
                    if results[mode][un.get("what")] != un.get(mode):
                        problems.append("%s: %s, want %s" % (mode, results[mode][un.get("what")], un.get(mode)))
                report(not problems, un.get("id"), "%s %s" % (un.get("what"), "; ".join(problems) if problems else
                                                              "unchanged through the wrapper, changed bare"))
            ag = ex.find("agent")
            w = results["wrapped"]
            lines, problems = w["agent_lines"], []
            first = lines[0] if lines else ""
            mm = re.match(r"^ok (\d+) windows?$", first)
            if not mm or w["agent_rc"] != 0:
                problems.append("first line %r exit %s, want 'ok N windows' and 0" % (first[:100], w["agent_rc"]))
            else:
                body = lines[1:]
                if int(mm.group(1)) != len(body):
                    problems.append("says %s windows, lists %d" % (mm.group(1), len(body)))
                listed = {l.split(" ")[0] for l in body}
                for item in listing.findall("item"):
                    win = windows[item.get("window")]
                    want = item.get("listed") == "true"
                    if (win["address"] in listed) != want:
                        problems.append("%s %s" % (item.get("window"), "missing from the listing" if want
                                                   else "listed, must not be"))
            if len(w["log"]) != 1:
                problems.append("%d new log lines, one expected" % len(w["log"]))
            else:
                rec = parse_log_line(w["log"][0])
                want_attrs = {"client": CALLER_CLIENT[caller], "action": "windows", "decision": "allow",
                              "rule": "per-window", "result": "done"}
                for k, v in want_attrs.items():
                    if rec is None or rec.get(k) != v:
                        problems.append("log %s=%r, want %r" % (k, rec.get(k) if rec is not None else None, v))
            report(not problems, ag.get("id"), "riley-agent windows through tools/riley-sandbox: " +
                   ("; ".join(problems) if problems else first))
        inst.kill(signal.SIGTERM)
    finally:
        transcript.close()
        inst.kill(signal.SIGKILL)
        for s in decoys:
            s.close()
        for p in holders:
            if p.poll() is None:
                os.kill(p.pid, signal.SIGTERM)
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.kill(p.pid, signal.SIGKILL)
                    p.wait(timeout=5)
    return finish("sandbox")


if __name__ == "__main__":
    sys.exit(main())
