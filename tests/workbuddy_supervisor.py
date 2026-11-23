"""Exercise production supervision against isolated native processes, never installed desktops."""
import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

BINARY, FIXTURE = (Path(arg).resolve() for arg in sys.argv[1:3])


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def wait_for(operation, description, timeout=25):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = operation()
            if last:
                return last
        except (OSError, ValueError):
            pass
        time.sleep(.1)
    raise AssertionError(f'{description}: last result {last!r}')


class Scenario:
    def __init__(self, *, enabled=True, automatic=False, account=True):
        self.temporary = tempfile.TemporaryDirectory(prefix='ca supervisor ')
        self.root = Path(self.temporary.name).resolve()
        self.home = self.root / 'home'
        self.executable = self.home / 'Applications/WorkBuddy.app/Contents/MacOS/Electron'
        self.executable.parent.mkdir(parents=True)
        shutil.copy2(FIXTURE, self.executable)
        self.environment = {**os.environ, 'HOME': str(self.home), 'CALLAGENT_FIXTURE_ROOT': str(self.root)}
        self.environment.pop('WORKBUDDY_REMOTE_DEBUGGING_PORT', None)
        self.debug_port = port()
        self.config = self.root / 'config.json'
        self.settings = {
            'version': 1, 'stateDirectory': str(self.root / 'runtime'), 'port': port(), 'refreshSeconds': 3600,
            'providers': {
                'codex': {'enabled': False, 'directory': str(self.home / '.codex')},
                'claude': {'enabled': False, 'directory': str(self.home / '.claude')},
                'workbuddy': {'enabled': True, 'directory': str(self.home / '.workbuddy'),
                              'debugPort': self.debug_port, 'autoEnableDebugPort': enabled}}, 'publishRoots': []}
        if not automatic:
            self.settings['providers']['workbuddy']['executable'] = str(self.executable)
        self.config.write_text(json.dumps(self.settings))
        self.processes = []
        self.client = None
        self.logs = []
        if account:
            self.login()

    def __enter__(self):
        return self

    def login(self):
        snapshot = self.home / '.workbuddy/storage/skeleton/account-snapshot.json'
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        snapshot.write_text(json.dumps({'primary': {'version': 1, 'uid': 'fixture-user'}}))

    def events(self, name='launch'):
        path = self.root / 'events.jsonl'
        return [row for line in (path.read_text().splitlines(keepends=True) if path.exists() else [])
                if line.endswith('\n') and (row := json.loads(line))['event'] == name]

    def status(self):
        result = subprocess.run([str(BINARY), '--config', str(self.config), 'status'],
                                capture_output=True, text=True, timeout=5)
        if result.returncode:
            return {}
        return json.loads(result.stdout)['workbuddySupervisor']

    def state(self, name, timeout=25):
        return wait_for(lambda: (value if (value := self.status()).get('state') == name else None), name, timeout)

    def start_client(self):
        log = (self.root / f'client-{len(self.logs)}.log').open('w+')
        self.logs.append(log)
        self.client = subprocess.Popen([str(BINARY), '--config', str(self.config), 'serve'],
                                       env=self.environment, cwd=self.root, stdout=log, stderr=log)
        wait_for(lambda: self.status(), 'Client did not start')

    def stop_client(self):
        if self.client and self.client.poll() is None:
            self.client.terminate()
            self.client.wait(timeout=10)
            assert self.client.returncode == 0
        self.client = None

    def manual_start(self, authorized=False):
        environment = dict(self.environment)
        if authorized:
            environment['WORKBUDDY_REMOTE_DEBUGGING_PORT'] = str(self.debug_port)
        child = subprocess.Popen([str(self.executable)], cwd=self.root, env=environment,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.processes.append(child)
        wait_for(lambda: any(row['pid'] == child.pid for row in self.events()), 'Desktop did not start')
        return child

    def user_exit(self, pid):
        (self.root / f'exit-{pid}').touch()
        wait_for(lambda: any(row['pid'] == pid for row in self.events('exit')), 'Desktop did not exit')

    def unchanged(self, count, seconds=6):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            assert len(self.events()) == count, self.events()
            time.sleep(.15)

    def __exit__(self, kind, value, traceback):
        self.stop_client()
        for row in self.events():
            (self.root / f'exit-{row["pid"]}').touch()
        for child in self.processes:
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        for row in self.events():
            if row['port'] and not (self.root / 'replacement-exits').exists():
                wait_for(lambda: any(event['pid'] == row['pid'] for event in self.events('exit')),
                         'Replacement fixture cleanup', timeout=5)
        if kind:
            for log in self.logs:
                log.flush(); log.seek(0); print(log.read(), file=sys.stderr)
        for log in self.logs:
            log.close()
        self.temporary.cleanup()


def lifecycle():
    with Scenario() as s:
        s.start_client()
        s.state('waiting_desktop')
        s.unchanged(0)
        original = s.manual_start()
        ready = s.state('ready')
        original.wait(timeout=5)
        assert ready['pid'] != original.pid and ready['restartCount'] == 1
        assert [row['port'] for row in s.events()] == [0, s.debug_port]
        s.unchanged(2)
        # Client exit must leave the user's replacement desktop running.
        s.stop_client()
        assert not any(row['pid'] == ready['pid'] for row in s.events('exit'))
        s.start_client()
        assert s.state('ready')['pid'] == ready['pid']
        s.user_exit(ready['pid'])
        s.state('waiting_desktop')
        s.unchanged(2)
        s.stop_client(); s.start_client(); s.state('waiting_desktop'); s.unchanged(2)


def conflicts():
    with Scenario() as s, socket.socket() as conflict:
        conflict.bind(('127.0.0.1', s.debug_port)); conflict.listen()
        original = s.manual_start(); s.start_client(); s.state('port_conflict')
        s.unchanged(1)
        assert original.poll() is None and not s.events('terminate')
        conflict.close()
        assert s.state('ready')['restartCount'] == 1


def discovery_and_disable():
    with Scenario(automatic=True, account=False) as s:
        original = s.manual_start(); s.start_client(); s.state('waiting_account')
        assert original.poll() is None and not s.events('terminate')
        s.login()
        ready = s.state('ready')
        assert ready['executable'] == str(s.executable) and ready['restartCount'] == 1
    with Scenario(enabled=False) as s:
        original = s.manual_start(); s.start_client(); s.state('disabled'); s.unchanged(1)
        assert original.poll() is None and not s.events('terminate')


def failed_replacement():
    with Scenario() as s:
        (s.root / 'replacement-exits').touch()
        original = s.manual_start(); s.start_client()
        wait_for(lambda: len(s.events()) == 2, 'Replacement not attempted')
        original.wait(timeout=5)
        s.state('waiting_desktop'); s.unchanged(2, seconds=12)
        s.stop_client(); s.start_client(); s.state('waiting_desktop'); s.unchanged(2)


def refused_exit():
    with Scenario() as s:
        (s.root / 'hold-exit').touch()
        original = s.manual_start(); s.start_client(); s.state('restart_not_completed')
        s.unchanged(1)
        assert original.poll() is None and len(s.events('terminate')) == 1


def cancelled_shutdown():
    with Scenario() as s:
        (s.root / 'hold-exit').touch()
        original = s.manual_start(); s.start_client()
        wait_for(lambda: s.events('terminate'), 'No graceful exit request')
        s.stop_client()
        s.user_exit(original.pid)
        s.unchanged(1)


def existing_listener():
    with Scenario() as s:
        (s.root / 'wrong-renderer').touch()
        original = s.manual_start(authorized=True); s.start_client(); s.state('waiting_ready')
        s.unchanged(1)
        assert original.poll() is None and not s.events('terminate')
        (s.root / 'wrong-renderer').unlink()
        ready = s.state('ready')
        assert ready['pid'] == original.pid and ready['restartCount'] == 0


def competing_clients():
    with Scenario() as first, Scenario() as second:
        desktop = first.manual_start(authorized=True)
        first.start_client(); first.state('ready')
        second.settings['providers']['workbuddy'] = dict(first.settings['providers']['workbuddy'])
        second.config.write_text(json.dumps(second.settings))
        second.start_client(); second.state('managed_elsewhere')
        first.unchanged(1)
        first.stop_client()
        assert second.state('ready')['pid'] == desktop.pid
        second.stop_client()
        assert desktop.poll() is None


def ambiguous_and_bound_port():
    with Scenario() as s:
        one, two = s.manual_start(), s.manual_start()
        s.start_client(); s.state('ambiguous_desktop')
        assert one.poll() is None and two.poll() is None and not s.events('terminate')
        s.user_exit(one.pid)
        assert s.state('ready')['restartCount'] == 1
    with Scenario() as s, socket.socket() as conflict:
        # A bound but non-listening socket does not appear in lsof's LISTEN output.
        conflict.bind(('127.0.0.1', s.debug_port))
        desktop = s.manual_start(); s.start_client(); s.state('port_conflict'); s.unchanged(1)
        assert desktop.poll() is None and not s.events('terminate')


def installation_scope():
    with Scenario() as s:
        desktop = s.manual_start()
        invalid = s.root / 'Other.app/Contents/MacOS/Electron'
        invalid.parent.mkdir(parents=True); shutil.copy2(FIXTURE, invalid)
        s.settings['providers']['workbuddy']['executable'] = str(invalid)
        s.config.write_text(json.dumps(s.settings))
        s.start_client(); s.state('invalid_installation')
        assert desktop.poll() is None and not s.events('terminate')


if __name__ == '__main__':
    # Each case has a different temporary bundle, account directory and ports.
    cases = [lifecycle, conflicts, discovery_and_disable, failed_replacement,
             refused_exit, cancelled_shutdown, existing_listener, competing_clients,
             ambiguous_and_bound_port, installation_scope]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        jobs = {executor.submit(case): case.__name__ for case in cases}
        for job in concurrent.futures.as_completed(jobs):
            job.result()
            print('PASS', jobs[job], flush=True)
