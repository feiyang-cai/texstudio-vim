#!/usr/bin/env python3
"""Exercise the shipped AppImage in an X11 desktop, not a build-tree binary."""
import os
from pathlib import Path
import signal
import subprocess
import time

root = Path('/work')
output = root / 'smoke-results' / os.environ['SMOKE_NAME']
output.mkdir(parents=True, exist_ok=True)
images = list((root / 'appimage').glob('*.AppImage'))
if len(images) != 1:
    raise SystemExit(f'Expected exactly one AppImage, found {len(images)}')
image = images[0]
image.chmod(0o755)
command = [str(image), '--appimage-extract-and-run']
version = subprocess.run(command + ['--version'], capture_output=True, text=True, timeout=60)
(output / 'version.log').write_text(version.stdout + version.stderr)
if version.returncode or os.environ['BUILD_SHA'][:7] not in version.stdout:
    raise SystemExit('Packaged binary has an unexpected version; see version.log')
config = output / 'config'
config.mkdir(exist_ok=True)
(config / 'texstudio.ini').write_text('[texmaker]\nEditor\\EditingMode=1\n')
fixture = output / 'vim-smoke.tex'
fixture.write_text('one\ntwo\nthree\n')
with (output / 'desktop.log').open('w') as log:
    process = subprocess.Popen(command + ['--start-always', '--disable-tests', '--config',
                               str(config), str(fixture)], stdout=log, stderr=subprocess.STDOUT,
                               start_new_session=True)
    try:
        window = subprocess.run(['xdotool', 'search', '--sync', '--onlyvisible', '--name',
                                 'vim-smoke[.]tex.*TeXstudio'], capture_output=True, text=True, timeout=30)
        if window.returncode or not window.stdout.strip():
            raise RuntimeError('No visible TeXstudio window')
        # The splash screen has the same WM_CLASS but vanishes during startup.
        # Wait for the loaded fixture's main window instead.
        window_id = window.stdout.splitlines()[0]
        subprocess.run(['xdotool', 'windowactivate', '--sync', window_id], check=True, timeout=10)
        time.sleep(2)
        if process.poll() is not None:
            raise RuntimeError('AppImage exited during startup')
        geometry = subprocess.run(['xdotool', 'getwindowgeometry', '--shell', window_id],
                                  capture_output=True, text=True, check=True)
        (output / 'window-geometry.log').write_text(geometry.stdout)
        dimensions = dict(line.split('=', 1) for line in geometry.stdout.splitlines() if '=' in line)
        # Activate the editor widget itself; top-level focus alone can leave a dock focused.
        subprocess.run(['xdotool', 'mousemove', '--window', window_id,
                        str(int(dimensions['WIDTH']) * 2 // 3),
                        str(int(dimensions['HEIGHT']) // 4), 'click', '1'], check=True)
        # Named yank, delete, and paste through the packaged application's keyboard path.
        subprocess.run(['xdotool', 'key', '--clearmodifiers', 'Escape'], check=True)
        subprocess.run(['xdotool', 'type', '--clearmodifiers', '--delay', '80', 'gg0"ayyjdd"aP'], check=True)
        subprocess.run(['xdotool', 'type', '--clearmodifiers', '--delay', '80', ':w'], check=True)
        subprocess.run(['xdotool', 'key', 'Return'], check=True)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and fixture.read_text().splitlines() != ['one', 'one', 'three']:
            time.sleep(.25)
        actual = fixture.read_text()
        (output / 'saved-text.log').write_text(actual)
        subprocess.run(['scrot', str(output / 'desktop.png')], check=True)
        if actual.splitlines() != ['one', 'one', 'three']:
            raise RuntimeError('Packaged Vim editing/save result is incorrect; see saved-text.log')
        if process.poll() is not None:
            raise RuntimeError('AppImage exited unexpectedly during editing')
        print('AppImage version, desktop startup, named registers, editing and save passed')
    finally:
        subprocess.run(['scrot', str(output / 'desktop.png')], check=False)
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
