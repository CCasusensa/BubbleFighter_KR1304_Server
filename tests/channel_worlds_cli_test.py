"""Exercise channel configuration through production startup; no database/UI."""
import json
import re
import socket
import subprocess
import time
from pathlib import Path

from runtime_smoke_test import GREETING, NO_WINDOW, receive_exactly

ROOT = Path(__file__).resolve().parents[1]


def main():
    reports = {}
    for config in ('Debug', 'Release'):
        executable = ROOT / 'Artifacts/Bin/x64' / config / 'BubbleFighter.Server.exe'
        directory = ROOT / 'Artifacts/smoke' / config / 'channel_rules'
        directory.mkdir(parents=True, exist_ok=True)
        checks = 0

        def require(value):
            nonlocal checks
            assert value, (config, checks)
            checks += 1

        cases = [('clan_without_service', '[80]\nMinimumLevel=1\nMaximumLevel=260\nMaximumMercenaries=1\n'),
                 ('clan', '[80]\nMinimumLevel=1\nMaximumLevel=260\n'),
                 ('reversed', '[20]\nMinimumLevel=20\nMaximumLevel=1\n'),
                 ('incomplete', '[70]\nMinimumLevel=21\n'),
                 ('duplicate', '[20]\nMinimumLevel=1\nMaximumLevel=20\n[20]\n')]
        for name, contents in cases:
            path = directory / (name + '.ini')
            path.write_text(contents, encoding='utf-8')
        for name, _ in [*cases, ('missing', '')]:
            path = directory / (name + '.ini')
            result = subprocess.run([str(executable), '--listen-port', '0', '--channel-rules', str(path)],
                                    cwd=ROOT, capture_output=True, timeout=10, creationflags=NO_WINDOW)
            (directory / (name + '.log')).write_bytes(result.stdout + result.stderr)
            require(result.returncode == 1 and b'Server error:' in result.stderr)
            require(b'Listening on' not in result.stdout)
        output_path = directory / 'valid.stdout.log'
        error_path = directory / 'valid.stderr.log'
        with output_path.open('wb') as output, error_path.open('wb') as error:
            process = subprocess.Popen([str(executable), '--listen-port', '0', '--once',
                                        '--channel-rules', 'config/examples/channel_rules.ini'],
                                       cwd=ROOT, stdout=output, stderr=error, creationflags=NO_WINDOW)
            try:
                match = None
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline and not match:
                    match = re.search(rb'Listening on 127\.0\.0\.1:(\d+)', output_path.read_bytes())
                    if not match:
                        assert process.poll() is None
                        time.sleep(0.025)
                require(match is not None)
                with socket.create_connection(('127.0.0.1', int(match[1])), timeout=5) as peer:
                    require(receive_exactly(peer, len(GREETING)) == GREETING)
                require(process.wait(timeout=10) == 0)
                require(not error_path.read_bytes())
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
        reports[config] = dict(checks=checks, passed=True, test_server_stopped=process.poll() is not None)
    report = dict(configurations=reports, database_used=False, original_ui=False)
    (ROOT / 'Artifacts/Logs/channel_worlds_cli.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
