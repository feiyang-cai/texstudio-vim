#!/usr/bin/env python3
"""Run native editor/save/remote TeX/PDF/SyncTeX integration with an SSH fixture."""
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
from ssh_fixture import ssh_fixture

parser = argparse.ArgumentParser()
parser.add_argument('executable')
parser.add_argument('--output', default='remote-test-results')
args = parser.parse_args()
output = Path(args.output).resolve(); output.mkdir(parents=True, exist_ok=True)
report = output/'remote-workspace-test-report.log'; report.unlink(missing_ok=True)
with ssh_fixture() as fixture:
    environment = os.environ.copy()
    environment.update(TXS_TEST_SSH_CONFIG=fixture['config'], TXS_TEST_REMOTE_ROOT=fixture['root'],
                       TXS_TEST_REMOTE_PYTHON='python' if os.name == 'nt' else 'python3',
                       QT_LOGGING_TO_CONSOLE='1', QT_FORCE_STDERR_LOGGING='1',
                       TEXSTUDIO_TEST_SCREENSHOT_DIR=str(output),
                       XDG_DATA_HOME=str(output/'data'), XDG_CACHE_HOME=str(output/'cache'),
                       XDG_CONFIG_HOME=str(output/'native-config'))
    with (output/'remote-gui-tests.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen([str(Path(args.executable).resolve()), '--start-always', '--config',
                                    str(output/'config'), '--auto-tests', '--remote-workspace-tests'],
                                   env=environment, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=os.name != 'nt')
        try:
            result = process.wait(timeout=240)
        except subprocess.TimeoutExpired:
            if os.name == 'nt': subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], check=False)
            else: os.killpg(process.pid, signal.SIGKILL)
            process.wait(); sys.exit('Remote desktop tests timed out; see remote-gui-tests.log')
text = (report if report.exists() else output/'remote-gui-tests.log').read_text(encoding='utf-8', errors='replace')
for line in text.splitlines():
    if any(word in line for word in ('Totals:', 'FAIL!', 'QFATAL')): print(line)
totals = re.findall(r'Totals: (\d+) passed, (\d+) failed, (\d+) skipped', text)
if result or len(totals) != 1 or any(int(failed) or int(skipped) for _, failed, skipped in totals):
    sys.exit(f'Remote desktop tests failed or coverage was incomplete (exit {result})')
for screenshot in ('remote-editor.png', 'remote-pdf-viewer.png', 'remote-rendered-page.png'):
    if not (output/screenshot).is_file(): sys.exit('Missing desktop evidence: '+screenshot)
