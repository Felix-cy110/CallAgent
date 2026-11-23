import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from desktop_fixture import Peers, write_json

binary = Path(sys.argv[1]).resolve()


def wait_for(operation, predicate, seconds=20):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        try:
            last = operation()
            if predicate(last):
                return last
        except (OSError, ValueError, subprocess.CalledProcessError):
            pass
        time.sleep(.05)
    raise AssertionError(f'Timed out, last result: {last}')


with tempfile.TemporaryDirectory(prefix='ca-', dir='/tmp') as temporary:
    root = Path(temporary).resolve()
    peers = Peers(root)
    config = root / 'config.json'
    wrapper = root / 'codex-reader'
    wrapper.write_text('#!/bin/sh\nexec ' + shlex.quote(sys.executable) + ' ' + shlex.quote(str(Path(__file__).with_name('desktop_fixture.py'))) +
                       ' --app-server ' + shlex.quote(str(root)) + ' "$@"\n')
    wrapper.chmod(0o700)
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    settings = {'version': 1, 'stateDirectory': str(root / 'state'), 'port': port, 'refreshSeconds': 3600,
        'providers': {
            'codex': {'enabled': True, 'directory': str(root / 'codex'), 'executable': str(wrapper), 'endpoint': str(peers.codex_socket)},
            'claude': {'enabled': True, 'directory': str(root / 'claude')},
            'workbuddy': {'enabled': True, 'directory': str(root / 'workbuddy'), 'executable': str(peers.executable), 'debugPort': peers.debug.server_port, 'autoEnableDebugPort': False}},
        'publishRoots': []}
    write_json(config, settings)

    def run(*args, check=True):
        result = subprocess.run([str(binary), '--config', str(config), *args], text=True, capture_output=True, timeout=25)
        if check and result.returncode:
            raise AssertionError(f'{args}: {result.stderr}')
        return json.loads(result.stdout) if result.returncode == 0 else result

    run('install-claude-bridge')
    run('publish', '--host', 'codex', '--directory', str(root / 'native-skills'))
    log = (root / 'daemon.log').open('w+')
    daemon = subprocess.Popen([str(binary), '--config', str(config), 'serve'], stdout=log, stderr=log)
    try:
        wait_for(lambda: (root / 'state/connection.json').exists(), bool)
        snapshot = wait_for(lambda: run('list'), lambda row: len(row['targets']) == 9)
        assert all(not row['stale'] for row in snapshot['providers'].values()), snapshot
        assert all(row['nativeId'] not in ('cli-only', 'wrong-account') for row in snapshot['targets'])
        assert {row['provider'] for row in snapshot['targets']} == {'codex', 'claude', 'workbuddy'}
        for provider in ('codex', 'claude', 'workbuddy'):
            items = [row for row in snapshot['targets'] if row['provider'] == provider]
            assert len(items) == 3 and items[0]['title'].endswith('3')
        generated = list((root / 'native-skills').glob('at-*/SKILL.md'))
        assert len(generated) == 9
        # Missing token and browser-origin requests are rejected even on loopback.
        endpoint = f'http://127.0.0.1:{port}/v1/request'
        for headers in ({}, {'Authorization': 'Bearer ' + json.loads((root / 'state/connection.json').read_text())['token'], 'Origin': 'https://example.invalid'}):
            try:
                urllib.request.urlopen(urllib.request.Request(endpoint, data=b'{"action":"status"}', headers=headers), timeout=3)
                raise AssertionError('Unauthorized HTTP request accepted')
            except urllib.error.HTTPError as error:
                assert error.code == 401
        # Competing daemon must not rotate credentials or scan/write catalogues.
        assert run('serve', check=False).returncode != 0
        peers.heartbeat()
        message = root / 'message.txt'
        message.write_text('explicit fixture send', encoding='utf-8')
        selected = {}
        for provider in ('codex', 'claude', 'workbuddy'):
            target = next(row for row in snapshot['targets'] if row['provider'] == provider)
            selected[provider] = target
            result = run('read', '--target', target['key'], '--limit', '10')
            assert len(result['messages']) == 2, result
            assert provider + ' fixture answer' in result['messages'][1]['text']
            assert 'hidden' not in json.dumps(result) and 'REASONING' not in json.dumps(result)
            assert len(peers.sent[provider]) == 0
            unauthorized = run('send', '--target', target['key'], '--request-id', 'test', '--text-file', str(message), check=False)
            assert unauthorized.returncode != 0
            result = run('send', '--target', target['key'], '--request-id', 'test', '--text-file', str(message), '--authorize')
            assert result['status'] in ('accepted', 'submitted', 'admitted'), result
            receipt = wait_for(lambda: run('receipt', '--target', target['key'], '--request-id', 'test'), lambda row: row['status'] in ('accepted', 'completed'))
            run('send', '--target', target['key'], '--request-id', 'test', '--text-file', str(message), '--authorize')
            assert len(peers.sent[provider]) == 1
        # Execute the generated helper with its bound identity.
        target = selected['codex']
        helper = root / 'native-skills' / ('at-codex-' + target['key'][:40]) / 'scripts/at'
        helper_result = subprocess.run([str(helper), 'read'], capture_output=True, text=True, timeout=25, check=True)
        assert json.loads(helper_result.stdout)['target']['key'] == target['key']
        peers.codex_busy = True
        busy = run('send', '--target', target['key'], '--request-id', 'busy', '--text-file', str(message), '--authorize')
        assert busy['status'] == 'rejected' and len(peers.sent['codex']) == 1
        peers.codex_busy = False
        # Lost acknowledgements remain uncertain and cannot be replayed with the same ID.
        peers.drop_codex_receipt = True
        for _ in range(2):
            result = run('send', '--target', target['key'], '--request-id', 'dropped', '--text-file', str(message), '--authorize')
            assert result['status'] == 'uncertain', result
        assert len(peers.sent['codex']) == 2
        # Account switching preserves the stale menu, but blocks reads and delivery.
        peers.account = 'another-account'
        assert run('read', '--target', selected['workbuddy']['key'], check=False).returncode != 0
        run('refresh')
        stale = wait_for(lambda: run('status'), lambda row: row['providers']['workbuddy']['stale'])
        assert len(stale['targets']) == 9
        print('Native Codex/Claude/WorkBuddy fixtures, daemon auth, generated entry and delivery invariants passed')
    finally:
        daemon.terminate()
        try:
            daemon.wait(timeout=15)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
        log.flush()
        if sys.exc_info()[0]:
            log.seek(0)
            print(log.read(), file=sys.stderr)
        log.close()
        peers.close()
        assert daemon.returncode == 0, f"Daemon shutdown failed: {daemon.returncode}"
