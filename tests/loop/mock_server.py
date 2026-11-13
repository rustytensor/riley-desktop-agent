# SPDX-License-Identifier: GPL-3.0-or-later
"""A made-up OpenAI-compatible chat-completions server for the M4 gate (tests/run-loop.sh). Standard library only.

It is not a model. It plays the scripted replies of one file in tests/loop/scripts/ in order, one per request to
POST /v1/chat/completions, and records every request it receives. Nothing it says comes from what it is sent.

    python3 -I mock_server.py --script FILE --vars FILE --record FILE --port-file FILE

--script     a script (tests/loop/scripts/*.json): {"case": "...", "replies": [REPLY, ...]}
             REPLY is {"tool_calls": [{"name": N, "arguments": {...}}, ...]} or {"content": "text"}; either may carry
             "delay_ms" (wait before answering). ${APP} and ${DECOY} in any argument string are replaced with the
             values in --vars (the Hyprland addresses of the gate's two test windows).
--vars       a JSON object of the values to put in, e.g. {"APP": "0x...", "DECOY": "0x..."}
--record     one JSON line per request, appended and flushed at once:
             {"n": N, "ms": T, "method": M, "path": P, "auth": A, "body": B, "reply": R}
             n counts from 1; ms is the wall clock in ms since the epoch; auth is null when there was no
             Authorization header, else {"sha256": hex of the header's value, "length": its length}: the header itself
             (a made-up test key) is never written; body is the request body parsed as JSON (or {"raw": text} when it
             is not JSON); reply is the index of the scripted reply sent, or null.
--port-file  the port it listens on (127.0.0.1, chosen by the kernel) is written here once it listens.

A request after the last scripted reply gets HTTP 500 {"error": "script ended"}; a request with "stream": true gets
HTTP 400 (the loop must not stream); any other path gets 404 (GET /v1/models lists one model, mock-model). Tool call
ids are "call-<case>-<request n>-<index>", so the gate knows them in advance. It runs until SIGTERM or SIGINT.
"""
import argparse
import hashlib
import json
import signal
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODEL = 'mock-model'


def substitute(value, names):
    if isinstance(value, str):
        for k, v in names.items():
            value = value.replace('${' + k + '}', v)
        return value
    if isinstance(value, list):
        return [substitute(v, names) for v in value]
    if isinstance(value, dict):
        return {k: substitute(v, names) for k, v in value.items()}
    return value


class Mock:
    def __init__(self, script, names, record):
        self.case = script['case']
        self.replies = [substitute(r, names) for r in script['replies']]
        self.record = open(record, 'a', encoding='utf-8')
        self.lock = threading.Lock()
        self.n = 0
        self.next_reply = 0

    def log(self, entry):
        self.record.write(json.dumps(entry, ensure_ascii=False) + '\n')
        self.record.flush()

    def completion(self, n, reply):
        if 'tool_calls' in reply:
            calls = [{'id': f'call-{self.case}-{n}-{i}', 'type': 'function',
                      'function': {'name': c['name'], 'arguments': json.dumps(c.get('arguments', {}))}}
                     for i, c in enumerate(reply['tool_calls'])]
            message = {'role': 'assistant', 'content': reply.get('content'), 'tool_calls': calls}
            finish = 'tool_calls'
        else:
            message = {'role': 'assistant', 'content': reply.get('content', '')}
            finish = 'stop'
        return {'id': f'chatcmpl-mock-{self.case}-{n}', 'object': 'chat.completion', 'created': int(time.time()),
                'model': MODEL, 'choices': [{'index': 0, 'message': message, 'finish_reason': finish}],
                'usage': {'prompt_tokens': 0, 'completion_tokens': 0, 'total_tokens': 0}}


def handler_for(mock):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, fmt, *args):   # no access log on stderr
            pass

        def send(self, status, obj):
            data = json.dumps(obj).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(data)
            self.close_connection = True

        def handle_any(self, method):
            length = int(self.headers.get('Content-Length') or 0)
            raw = self.rfile.read(length) if length > 0 else b''
            try:
                body = json.loads(raw) if raw else None
            except ValueError:
                body = {'raw': raw.decode('utf-8', errors='replace')}
            auth = self.headers.get('Authorization')
            auth_rec = None if auth is None else {'sha256': hashlib.sha256(auth.encode()).hexdigest(),
                                                  'length': len(auth)}
            path = self.path.split('?', 1)[0]
            with mock.lock:
                mock.n += 1
                n = mock.n
                index = None
                status, answer, delay = 404, {'error': 'not found'}, 0
                if method == 'POST' and path == '/v1/chat/completions':
                    if isinstance(body, dict) and body.get('stream'):
                        status, answer = 400, {'error': 'this mock does not stream'}
                    elif mock.next_reply >= len(mock.replies):
                        status, answer = 500, {'error': 'script ended'}
                    else:
                        index = mock.next_reply
                        mock.next_reply += 1
                        reply = mock.replies[index]
                        status, answer, delay = 200, mock.completion(n, reply), reply.get('delay_ms', 0)
                elif method == 'GET' and path == '/v1/models':
                    status, answer = 200, {'object': 'list', 'data': [{'id': MODEL, 'object': 'model'}]}
                mock.log({'n': n, 'ms': int(time.time() * 1000), 'method': method, 'path': path, 'auth': auth_rec,
                          'body': body, 'reply': index})
            if delay:
                time.sleep(delay / 1000)
            self.send(status, answer)

        def do_POST(self):
            self.handle_any('POST')

        def do_GET(self):
            self.handle_any('GET')

    return Handler


def main():
    p = argparse.ArgumentParser(description='made-up OpenAI-compatible server for the M4 gate')
    p.add_argument('--script', required=True)
    p.add_argument('--vars', required=True)
    p.add_argument('--record', required=True)
    p.add_argument('--port-file', required=True)
    a = p.parse_args()
    with open(a.script, encoding='utf-8') as f:
        script = json.load(f)
    with open(a.vars, encoding='utf-8') as f:
        names = json.load(f)
    mock = Mock(script, names, a.record)
    server = ThreadingHTTPServer(('127.0.0.1', 0), handler_for(mock))
    server.daemon_threads = True

    def end(signum, frame):
        threading.Thread(target=server.shutdown, daemon=True).start()
    signal.signal(signal.SIGTERM, end)
    signal.signal(signal.SIGINT, end)
    with open(a.port_file, 'w') as f:
        f.write(str(server.server_address[1]))
    server.serve_forever(poll_interval=0.1)
    server.server_close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
