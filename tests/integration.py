#!/usr/bin/env python3
"""Real loopback SSH + tmux test, with a deterministic Claude stand-in.

Run on a disposable Linux test server as root. Does not authenticate Claude,
call a model, alter SSH configuration, or listen on a public interface.
"""
import json
import os
from pathlib import Path
import pty
import shutil
import signal
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, text=True, **kwargs)


def wait_for(predicate, description):
    for _ in range(100):
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError('timeout: ' + description)


def main():
    if os.geteuid() != 0:
        raise SystemExit('Run only on a disposable Linux test server as root')
    for binary in ['sshd', 'ssh', 'ssh-keygen', 'tmux', 'flock', 'shasum']:
        if not shutil.which(binary):
            raise SystemExit('Missing prerequisite: ' + binary)
    children = []
    with tempfile.TemporaryDirectory(prefix='ccproxy-integration-') as directory:
        base = Path(directory)
        workspace = base / 'project'
        workspace.mkdir()
        events = base / 'events.jsonl'
        session = 'ccproxy-test-' + str(os.getpid())
        state = Path.home() / '.claude/remote-proxy' / (session + '.session-id')
        key_dir = Path.home() / '.claude/projects' / str(workspace).replace('/', '-')
        run('ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(base / 'key'))
        run('ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(base / 'host'))
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        config = base / 'sshd_config'
        config.write_text(f'''Port {port}
ListenAddress 127.0.0.1
HostKey {base}/host
PidFile {base}/sshd.pid
AuthorizedKeysFile {base}/key.pub
StrictModes no
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitRootLogin prohibit-password
UsePAM no
AllowUsers root
''')
        pub = (base / 'host.pub').read_text().split()
        (base / 'known_hosts').write_text(f'[127.0.0.1]:{port} {pub[0]} {pub[1]}\n')
        (base / 'ssh_config').write_text(f'''Host test-company
 HostName 127.0.0.1
 Port {port}
 User root
 IdentityFile {base}/key
 IdentitiesOnly yes
 StrictHostKeyChecking yes
 UserKnownHostsFile {base}/known_hosts
 BatchMode yes
''')
        bin_dir = base / 'bin'
        bin_dir.mkdir()
        wrapper = bin_dir / 'ssh'
        wrapper.write_text(f'#!/bin/sh\nexec /usr/bin/ssh -F {base}/ssh_config "$@"\n')
        wrapper.chmod(0o755)
        standin = base / 'claude-test'
        standin.write_text('''#!/usr/bin/python3
import json,sys,os,time
from pathlib import Path
events=Path(''' + repr(str(events)) + ''')
uuid=sys.argv[-1]
key=str(Path.cwd()).replace('/', '-')
target=Path.home()/'.claude/projects'/key
target.mkdir(parents=True,exist_ok=True)
(target/(uuid+'.jsonl')).touch()
with events.open('a') as f:
 f.write(json.dumps({'pid':os.getpid(),'args':sys.argv[1:]})+'\\n')
print('TEST_CLAUDE_READY',flush=True)
while True: time.sleep(1)
''')
        standin.chmod(0o755)
        env = dict(os.environ, PATH=str(bin_dir) + ':' + os.environ['PATH'],
                   TERM='xterm-256color', CLAUDE_PROXY_MODE='remote',
                   CLAUDE_PROXY_PROJECT_DIR=str(workspace), SSH_TARGET='test-company',
                   REMOTE_USER='root', REMOTE_PROJECT_DIR=str(workspace),
                   REMOTE_TMUX_SESSION=session, REMOTE_CLAUDE_BIN=str(standin),
                   CLAUDE_PROXY_SOCKET=str(base / 'control.sock'))
        daemon = subprocess.Popen(['/usr/sbin/sshd', '-D', '-f', str(config), '-E', str(base / 'sshd.log')])
        children.append(daemon)

        def ready():
            with socket.socket() as probe:
                return probe.connect_ex(('127.0.0.1', port)) == 0

        def records():
            return [json.loads(line) for line in events.read_text().splitlines()] if events.exists() else []

        def attach():
            master, slave = pty.openpty()
            process = subprocess.Popen(['bash', str(ROOT / 'bin/claude-remote-proxy'), 'start'],
                                       env=env, stdin=slave, stdout=slave, stderr=slave,
                                       start_new_session=True)
            os.close(slave)
            children.append(process)
            return process, master

        def drop(process, terminal):
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGHUP)
                process.wait(timeout=10)
            os.close(terminal)

        try:
            wait_for(ready, 'private SSH daemon')
            first, terminal = attach()
            wait_for(lambda: len(records()) == 1, 'first remote launch')
            initial = records()[0]
            assert initial['args'][0] == '--session-id'
            drop(first, terminal)
            run('tmux', 'has-session', '-t', session)
            second, terminal = attach()
            time.sleep(1)
            assert second.poll() is None, 'reconnect did not stay attached'
            assert records() == [initial], 'reconnect launched another Claude process'
            drop(second, terminal)
            run('tmux', 'kill-session', '-t', session)
            third, terminal = attach()
            wait_for(lambda: len(records()) == 2, 'resume after process exit')
            restarted = records()[1]
            assert restarted['args'] == ['--resume', initial['args'][-1]]
            drop(third, terminal)
            print(json.dumps({'result': 'PASS', 'transport': 'real SSH + tmux',
                              'reconnect_preserved_pid': initial['pid'],
                              'restart_resumed_same_uuid': True,
                              'model_calls': 0}))
        finally:
            subprocess.run(['tmux', 'kill-session', '-t', session], capture_output=True)
            subprocess.run(['ssh', '-S', str(base / 'control.sock'), '-O', 'exit', 'test-company'],
                           env=env, capture_output=True)
            for child in children:
                if child.poll() is None:
                    child.terminate()
                    child.wait(timeout=10)
            state.unlink(missing_ok=True)
            state.with_suffix('.session-id.lock').unlink(missing_ok=True)
            if key_dir.exists():
                shutil.rmtree(key_dir)


if __name__ == '__main__':
    main()
