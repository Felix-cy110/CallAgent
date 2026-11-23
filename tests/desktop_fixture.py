"""Native protocol peers; no real Desktop, account, model call or CLI turn is used."""
import base64
import ctypes
import hashlib
import hmac
import json
import os
from pathlib import Path
import socket
import sqlite3
import struct
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.temporary')
    temporary.write_text(json.dumps(value), encoding='utf-8')
    temporary.replace(path)


def native_reader(root):
    for line in sys.stdin:
        frame = json.loads(line)
        if 'id' not in frame:
            continue
        config = json.loads((root / 'fixture.json').read_text())
        method = frame['method']
        if method == 'initialize':
            result = {}
        elif method == 'thread/list':
            result = {'data': config['codex'], 'nextCursor': None}
        elif method == 'thread/read':
            result = {'thread': {'id': frame['params']['threadId'], 'historyMode': 'paginated'}}
        elif method == 'thread/turns/list':
            assert frame['params']['itemsView'] == 'full'
            result = {'data': [{'id': 'turn-fixture', 'status': 'completed', 'items': [
                {'type': 'userMessage', 'content': [{'type': 'text', 'text': 'codex fixture question'}]},
                {'type': 'reasoning', 'text': 'DO NOT EXPOSE PRIVATE REASONING'},
                {'type': 'agentMessage', 'text': 'codex fixture answer'}]}]}
        else:
            raise AssertionError('Unexpected native reader mutation: ' + method)
        print(json.dumps({'id': frame['id'], 'result': result}), flush=True)


def proof(ticket, role, endpoint, client, server):
    data = b''.join(struct.pack('!I', len(x.encode())) + x.encode() for x in [role, '2', endpoint, client, server])
    return base64.urlsafe_b64encode(hmac.new(ticket.encode(), data, hashlib.sha256).digest()).decode().rstrip('=')


class Peers:
    def __init__(self, root):
        self.root = root
        self.sent = {'codex': [], 'claude': [], 'workbuddy': []}
        self.lock = threading.Lock()
        self.rpc_rows = {}
        self.drop_codex_receipt = False
        self.codex_busy = False
        self.account = 'fixture-user'
        self.sockets = []
        self.codex_socket = root / 'codex/ipc/ipc.sock'
        self.claude_socket = root / 'claude-peer.sock'
        self.start_unix(self.codex_socket, self.codex)
        self.start_unix(self.claude_socket, self.claude)
        peers = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *_):
                pass

            def do_GET(self):
                if self.path == '/json/list':
                    data = json.dumps([{'type': 'page', 'title': 'WorkBuddy', 'url': peers.renderer.as_uri() + '?locale=zh-CN&fixtureToken=not-a-real-token',
                                        'webSocketDebuggerUrl': f'ws://127.0.0.1:{peers.debug.server_port}/devtools/page/fixture'}]).encode()
                    self.send_response(200)
                    self.send_header('Content-Length', str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                    return
                self.send_response(101)
                self.send_header('Upgrade', 'websocket')
                self.send_header('Connection', 'Upgrade')
                key = self.headers['Sec-WebSocket-Key']
                self.send_header('Sec-WebSocket-Accept', base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode())
                if self.path == '/rpc':
                    self.send_header('Sec-WebSocket-Protocol', 'workbuddy.local-rpc.v2')
                self.end_headers()
                self.connection.settimeout(30)
                try:
                    self.websocket()
                except (EOFError, OSError, BrokenPipeError):
                    pass
                self.close_connection = True

            def read_frame(self):
                head = self.rfile.read(2)
                if len(head) != 2:
                    raise EOFError()
                op, size = head[0] & 15, head[1] & 127
                if size == 126:
                    size = struct.unpack('!H', self.rfile.read(2))[0]
                elif size == 127:
                    size = struct.unpack('!Q', self.rfile.read(8))[0]
                assert size <= 8 * 1024 * 1024
                mask = self.rfile.read(4) if head[1] & 128 else b''
                data = self.rfile.read(size)
                if mask:
                    data = bytes(c ^ mask[i % 4] for i, c in enumerate(data))
                if op == 8:
                    raise EOFError()
                if op == 9:
                    self.send_frame(data, 10)
                    return self.read_frame()
                assert op == 1
                return json.loads(data)

            def send_frame(self, frame, op=1):
                data = json.dumps(frame).encode() if op == 1 else frame
                length = len(data)
                head = bytes([128 | op, length]) if length < 126 else bytes([128 | op, 126]) + struct.pack('!H', length)
                self.wfile.write(head + data)
                self.wfile.flush()

            def websocket(self):
                if self.path != '/rpc':
                    request = self.read_frame()
                    assert request['method'] == 'Runtime.evaluate'
                    assert request['params']['expression'] == 'window.__wbInvoke("wb:extensions:$resolveLocalRpc",undefined,"edge-sync","invokeSendPrompt")'
                    self.send_frame({'id': request['id'], 'result': {'result': {'value': {
                        'extensionId': 'edge-sync', 'protocolVersion': 2, 'endpoint': peers.endpoint,
                        'ticket': 'fixture-ticket', 'methods': peers.methods}}}})
                    while True:
                        self.read_frame()
                hello = self.read_frame()
                assert hello['type'] == 'session_hello'
                assert hello['ticket_id'] == hashlib.sha256(b'fixture-ticket').hexdigest()[:16]
                nonce = hello['client_nonce']
                self.send_frame({'type': 'session_challenge', 'protocol': 2, 'server_nonce': 'server',
                    'server_proof': proof('fixture-ticket', 'wbipc-s', peers.endpoint, nonce, 'server')})
                response = self.read_frame()
                assert response['client_proof'] == proof('fixture-ticket', 'wbipc-c', peers.endpoint, nonce, 'server')
                self.send_frame({'type': 'session_hello_ack', 'methods': peers.methods})
                while True:
                    request = self.read_frame()
                    result = peers.rpc(request['method'], request['params'])
                    self.send_frame({'jsonrpc': '2.0', 'id': request['id'], 'result': result})

        self.methods = ['edge-sync/invokeWbAny', 'edge-sync/invokeGetInfo', 'edge-sync/invokeSendPrompt']
        buffer = ctypes.create_string_buffer(4096)
        assert ctypes.CDLL('/usr/lib/libproc.dylib').proc_pidpath(os.getpid(), buffer, len(buffer)) > 0
        self.executable = Path(buffer.value.decode()).resolve()
        self.renderer = (self.executable.parent / '../Resources/app.asar/renderer/index.html').resolve()
        self.debug = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.extension = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.endpoint = f'ws://127.0.0.1:{self.extension.server_port}/rpc'
        for server in [self.debug, self.extension]:
            threading.Thread(target=server.serve_forever, daemon=True).start()
        self.populate()

    def start_unix(self, path, handler):
        path.parent.mkdir(parents=True, exist_ok=True)
        server = socket.socket(socket.AF_UNIX)
        server.bind(str(path))
        server.listen()
        self.sockets.append(server)
        def accept():
            while True:
                try:
                    connection, _ = server.accept()
                except OSError:
                    return
                def serve(connection=connection):
                    try:
                        with connection:
                            handler(connection)
                    except (EOFError, OSError):
                        pass
                threading.Thread(target=serve, daemon=True).start()
        threading.Thread(target=accept, daemon=True).start()

    def codex(self, connection):
        stream = connection.makefile('rb')
        while True:
            prefix = stream.read(4)
            if len(prefix) != 4:
                return
            request = json.loads(stream.read(struct.unpack('<I', prefix)[0]))
            method = request['method']
            result, owner, error = {}, 'desktop-owner', None
            if method == 'thread-stream-following-changed':
                assert request['type'] == 'broadcast'
                if not request['params']['following']:
                    continue
                sid = request['params']['conversationId']
                data = json.dumps({'type': 'broadcast', 'method': 'thread-stream-state-changed', 'version': 13,
                    'sourceClientId': owner, 'params': {'conversationId': sid, 'hostId': 'local', 'change': {
                        'type': 'snapshot', 'revision': 1, 'conversationState': {'id': sid, 'turns': [], 'requests': [],
                        'threadRuntimeStatus': {'type': 'active' if self.codex_busy else 'idle'}}}}}).encode()
                connection.sendall(struct.pack('<I', len(data)) + data)
                continue
            if method == 'initialize':
                assert request['version'] == 0
                result = {'clientId': 'follower'}
            elif method == 'thread-owner-discovery':
                if request['params']['conversationId'] == 'cli-only':
                    error = 'no-client-found'
            elif method == 'thread-follower-start-turn':
                assert request['version'] == 2 and request['targetClientId'] == owner
                assert request['params']['turnStart']['context']['inheritThreadSettings'] is True
                with self.lock:
                    self.sent['codex'].append(request['params'])
                if self.drop_codex_receipt:
                    return
                result = {'result': {'turn': {'id': 'turn-fixture'}}}
            else:
                raise AssertionError(method)
            frame = {'type': 'response', 'requestId': request['requestId'], 'method': method,
                     'handledByClientId': owner, 'resultType': 'error' if error else 'success', 'result': result}
            if error:
                frame['error'] = error
            data = json.dumps(frame).encode()
            connection.sendall(struct.pack('<I', len(data)) + data)

    def claude(self, connection):
        stream = connection.makefile('rb')
        assert json.loads(stream.readline()) == {'type': 'auth', 'token': 'a' * 32}
        message = json.loads(stream.readline())
        assert uuid.UUID(message['uuid']).version == 4 and message['msg_id'] == message['uuid']
        payload = message['message']['content']
        assert payload.startswith('CALLAGENT_SESSION_V1:')
        frame = json.loads(payload.split(':', 1)[1])
        request = json.loads((self.root / 'claude/callagent-desktop-bridge/requests' / (frame['id'] + '.json')).read_text())
        assert request['token'] == frame['token'] and request['sessionId'] == frame['sessionId']
        with self.lock:
            self.sent['claude'].append(request['text'])
        write_json(self.root / 'claude/callagent-desktop-bridge/receipts' / (frame['id'] + '.json'),
                   {'version': '1', 'id': frame['id'], 'sessionId': frame['sessionId'], 'status': 'accepted'})

    def rpc(self, method, params):
        if method.endswith('/invokeWbAny'):
            channel, args = params['channel'], json.loads(params['argsJson'])
            if channel == 'wb:account:profile':
                result = {'uid': self.account}
            elif channel == 'wb:conversations:requests':
                result = {'items': self.rpc_rows.get(args[0], [{'id': 'old', 'state': 'completed',
                    'userMessage': {'content': [{'type': 'text', 'text': 'workbuddy fixture question'}]},
                    'assistantMessage': {'content': [{'type': 'text', 'text': 'workbuddy fixture answer'}]}}])}
            elif channel == 'wb:conversations:getPendingInfo':
                result = None
            else:
                raise AssertionError(channel)
            return {'ok': True, 'data': {'result': result}}
        if method.endswith('/invokeGetInfo'):
            return {'ok': True, 'data': {'info': {'id': params['conversationId'], 'transport': 'local', 'space': {'cwd': '/fixture'}}}}
        assert method.endswith('/invokeSendPrompt')
        with self.lock:
            self.sent['workbuddy'].append(params)
        self.rpc_rows[params['conversationId']] = [{'id': params['options']['_meta']['codebuddy.ai/conversationRequestId'],
            'clientRequestId': params['options']['clientRequestId'], 'state': 'completed'}]
        return {'ok': True, 'data': {'conversationId': params['conversationId'], 'result': {}}}

    def populate(self):
        now = int(time.time())
        self.codex_rows = [{'id': f'c{i}', 'name': f'Codex {i}', 'cwd': '/fixture', 'updatedAt': now + i} for i in range(4)]
        self.codex_rows.append(dict(self.codex_rows[-1]))  # Native pagination may overlap.
        self.codex_rows.append({'id': 'cli-only', 'name': 'CLI must be excluded', 'cwd': '/fixture', 'updatedAt': now + 10})
        write_json(self.root / 'fixture.json', {'codex': self.codex_rows})
        for i in range(4):
            sid = f'00000000-0000-0000-0000-{i:012d}'
            write_json(self.root / f'claude/sessions/{i}.json', {'sessionId': sid, 'title': f'Claude {i}', 'cwd': '/fixture',
                'entrypoint': 'claude-desktop', 'hostSessionId': f'local_{i}', 'pid': os.getpid(), 'status': 'idle', 'messagingSocketPath': str(self.claude_socket)})
            path = self.root / f'claude/projects/-fixture/{sid}.jsonl'
            path.parent.mkdir(parents=True, exist_ok=True)
            rows = [dict(uuid='first', parentUuid=None, sessionId=sid, type='user', message={'content': 'claude fixture question'}),
                    dict(uuid='second', parentUuid='first', sessionId=sid, type='assistant', message={'content': [{'type': 'thinking', 'thinking': 'hidden'}, {'type': 'text', 'text': 'claude fixture answer'}]}),
                    dict(uuid='side', parentUuid='first', sessionId=sid, type='assistant', isSidechain=True, message={'content': 'hidden sidechain'})]
            path.write_text(''.join(json.dumps(row) + '\n' for row in rows))
            os.utime(path, (now + i, now + i))
        credential = hashlib.sha256(str(self.claude_socket).lower().encode()).hexdigest()
        write_json(self.root / f'claude/sessions/{os.getpid()}.{credential}.key', {'peerToken': 'a' * 32})
        write_json(self.root / 'workbuddy/storage/skeleton/account-snapshot.json', {'primary': {'version': 1, 'uid': self.account}})
        db = sqlite3.connect(self.root / 'workbuddy/workbuddy.db')
        db.execute('CREATE TABLE sessions(id TEXT,cwd TEXT,custom_title TEXT,title TEXT,updated_at INTEGER,user_id TEXT,transport TEXT,deleted_at INTEGER)')
        for i in range(4):
            db.execute('INSERT INTO sessions VALUES(?,?,?,?,?,?,?,NULL)', (f'w{i}', '/fixture', None, f'WorkBuddy {i}', (now + i) * 1000, self.account, 'local'))
        db.execute('INSERT INTO sessions VALUES(?,?,?,?,?,?,?,NULL)', ('wrong-account', '/fixture', None, 'excluded', (now + 100) * 1000, 'other', 'local'))
        db.commit()
        db.close()

    def heartbeat(self):
        control = self.root / 'claude/callagent-desktop-bridge'
        lease = json.loads((control / 'connection.json').read_text())
        for i in range(4):
            sid = f'00000000-0000-0000-0000-{i:012d}'
            write_json(control / 'heartbeats' / (sid + '.json'), {'version': '1', 'bundle': lease['bundle'], 'challenge': lease['challenge'],
                'sessionId': sid, 'engineVersion': '2.1.309', 'observedAt': int(time.time() * 1000)})

    def close(self):
        for server in [self.debug, self.extension]:
            server.shutdown()
            server.server_close()
        for server in self.sockets:
            server.close()


if __name__ == '__main__':
    assert sys.argv[1] == '--app-server'
    native_reader(Path(sys.argv[2]))
