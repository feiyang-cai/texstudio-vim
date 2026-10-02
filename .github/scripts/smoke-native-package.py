#!/usr/bin/env python3
"""Drive a packaged application through the host desktop's real input APIs."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import zipfile

import pyautogui as gui

input_driver = gui

parser = argparse.ArgumentParser()
parser.add_argument('--packages', default='native-package')
parser.add_argument('--output', default='native-smoke-results')
args = parser.parse_args()
output = Path(args.output).resolve()
output.mkdir(parents=True, exist_ok=True)
archives = list(Path(args.packages).rglob('*.zip'))
if len(archives) != 1:
    raise SystemExit(f'Expected one portable package, got {len(archives)}')
installation = Path(args.packages).resolve() / 'installation'
installation.mkdir(exist_ok=True)
if sys.platform == 'darwin':
    subprocess.run(['ditto', '-x', '-k', str(archives[0]), str(installation)], check=True)
    executable, = installation.glob('*.app/Contents/MacOS/texstudio')
    import Quartz
    from AppKit import NSRunningApplication, NSApplicationActivateIgnoringOtherApps
    if hasattr(Quartz, 'CGPreflightPostEventAccess') and not Quartz.CGPreflightPostEventAccess():
        (output / 'result.json').write_text(json.dumps({'status': 'blocked', 'reason': 'macOS desktop input permission unavailable'}))
        raise SystemExit('Desktop input permission unavailable: macOS Accessibility authorization is required')
else:
    with zipfile.ZipFile(archives[0]) as archive:
        archive.extractall(installation)
    executable, = installation.rglob('texstudio.exe')
    from windows_desktop_input import WindowsDesktopInput
    input_driver = WindowsDesktopInput()
    user32 = ctypes.windll.user32
    # Windows desktop handles are pointer sized on both x64 and ARM64.
    user32.GetForegroundWindow.restype = ctypes.c_void_p
    user32.GetWindowTextLengthW.argtypes = [ctypes.c_void_p]
    user32.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    user32.SetForegroundWindow.argtypes = [ctypes.c_void_p]
    user32.ShowWindow.argtypes = [ctypes.c_void_p, ctypes.c_int]

version = subprocess.run([str(executable), '--version'], capture_output=True, text=True, timeout=60)
(output / 'version.log').write_text(version.stdout + version.stderr)
if version.returncode or os.environ['BUILD_SHA'][:7] not in version.stdout:
    raise SystemExit('Packaged executable has an unexpected version; see version.log')

fixture = output / 'vim-native-smoke.tex'
fixture.write_text('one\ntwo\nthree\n', encoding='utf-8')
config = output / 'config'
config.mkdir(exist_ok=True)
# No editing-mode override: this must verify Vim is the default.
(config / 'texstudio.ini').write_text('[texmaker]\nStartup\\CheckLatexConfiguration=false\n')
command = [str(executable), '--start-always', '--disable-tests', '--config', str(config), str(fixture)]

def activate(pid):
    if sys.platform == 'darwin':
        application = NSRunningApplication.runningApplicationWithProcessIdentifier_(pid)
        if application:
            application.activateWithOptions_(NSApplicationActivateIgnoringOtherApps)
        for window in Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionOnScreenOnly, Quartz.kCGNullWindowID):
            if window.get('kCGWindowOwnerPID') == pid and window.get('kCGWindowLayer') == 0:
                name = window.get('kCGWindowName', '')
                bounds = window['kCGWindowBounds']
                if 'vim-native-smoke.tex' in name:
                    return bounds['X'], bounds['Y'], bounds['Width'], bounds['Height']
    else:
        result = []
        callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
        @callback_type
        def callback(hwnd, _):
            owner = ctypes.c_ulong()
            user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
            if owner.value == pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
                title = ctypes.create_unicode_buffer(user32.GetWindowTextLengthW(hwnd) + 1)
                user32.GetWindowTextW(hwnd, title, len(title))
                if 'vim-native-smoke.tex' in title.value:
                    input_driver.activate(hwnd)
                    from ctypes.wintypes import RECT
                    bounds = RECT()
                    user32.GetWindowRect(ctypes.c_void_p(hwnd), ctypes.byref(bounds))
                    result.append((bounds.left, bounds.top, bounds.right-bounds.left, bounds.bottom-bounds.top))
            return True
        user32.EnumWindows(callback, 0)
        if result:
            return result[0]
    return None

def ensure_windows_input_focus():
    if sys.platform == 'darwin':
        return
    api = ctypes.windll.user32
    api.GetForegroundWindow.restype = ctypes.c_void_p
    foreground = api.GetForegroundWindow()
    owner = ctypes.c_ulong()
    api.GetWindowThreadProcessId(foreground, ctypes.byref(owner))
    if owner.value == process.pid:
        return
    api.GetWindowTextLengthW.argtypes = [ctypes.c_void_p]
    api.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    title = ctypes.create_unicode_buffer(api.GetWindowTextLengthW(foreground) + 1)
    api.GetWindowTextW(foreground, title, len(title))
    gui.screenshot().save(output / 'interrupted-desktop.png')
    # A delayed first-login WSL installer can steal focus after startup passed.
    # Close only its known setup window on this disposable hosted CI machine.
    if 'wsl.exe' not in title.value.lower():
        raise RuntimeError(f'Unexpected desktop focus loss: pid={owner.value}, title={title.value!r}')
    print('Closing delayed hosted-runner WSL setup:', title.value, flush=True)
    api.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
    api.PostMessageW(foreground, 0x0010, 0, 0) # WM_CLOSE
    time.sleep(.5)
    geometry = activate(process.pid)
    if not geometry:
        raise RuntimeError('Application window missing after closing runner setup')
    x, y, width, height = geometry
    input_driver.click(x + width * 2 // 3, y + height // 4)
    time.sleep(.2)
    api.GetWindowThreadProcessId(api.GetForegroundWindow(), ctypes.byref(owner))
    if owner.value != process.pid:
        raise RuntimeError('Application did not regain desktop focus')

def type_keys(text):
    ensure_windows_input_focus()
    input_driver.write(text, interval=.09)

def save_and_check(label, expected):
    type_keys(':w')
    input_driver.press('enter')
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        actual = fixture.read_text(encoding='utf-8').splitlines()
        if actual == expected:
            (output / f'{label}.txt').write_text('\n'.join(actual), encoding='utf-8')
            return
        time.sleep(.2)
    (output / 'unexpected-saved-text.txt').write_text('\n'.join(actual), encoding='utf-8')
    raise RuntimeError(f'{label}: desktop input or editing failed; expected {expected!r}, got {actual!r}')

with (output / 'application.log').open('w') as log:
    process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 45
        geometry = None
        while not geometry and time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError('Packaged application exited during startup')
            geometry = activate(process.pid)
            time.sleep(.5)
        if not geometry:
            raise RuntimeError('No accessible fixture window; check desktop session and macOS screen-recording permission')
        # The title appears before startup finishes arranging the editor/docks.
        # Re-read geometry after startup so the click uses the final editor area.
        time.sleep(3)
        if sys.platform != 'darwin':
            # First-login setup can leave the Start menu over the app window.
            input_driver.press('esc')
            time.sleep(.2)
        geometry = activate(process.pid)
        if not geometry:
            raise RuntimeError('Fixture window disappeared during startup')
        (output / 'window.json').write_text(json.dumps(geometry))
        x, y, width, height = geometry
        input_driver.click(x + width * 2 // 3, y + height // 4)
        time.sleep(.5)
        gui.screenshot().save(output / 'before-input.png')
        if sys.platform != 'darwin':
            owner = ctypes.c_ulong()
            foreground = user32.GetForegroundWindow()
            user32.GetWindowThreadProcessId(ctypes.c_void_p(foreground), ctypes.byref(owner))
            title = ctypes.create_unicode_buffer(user32.GetWindowTextLengthW(foreground) + 1)
            user32.GetWindowTextW(foreground, title, len(title))
            details = {'expected_pid': process.pid, 'foreground_pid': owner.value,
                       'foreground_title': title.value, 'activation': input_driver.activation}
            kernel = ctypes.windll.kernel32
            kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_ulong)]
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            handle = kernel.OpenProcess(0x1000, False, owner.value)
            if handle:
                buffer = ctypes.create_unicode_buffer(2048)
                length = ctypes.c_ulong(len(buffer))
                if kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(length)):
                    details['foreground_image'] = buffer.value
                kernel.CloseHandle(handle)
            (output / 'focus.json').write_text(json.dumps(details))
            print('Desktop focus:', json.dumps(details))
            if owner.value != process.pid:
                raise RuntimeError('Desktop focus did not reach the packaged application')
        input_driver.press('esc')
        type_keys('gg0"ayyjdd"aP')
        save_and_check('default-vim-registers', ['one', 'one', 'three'])
        type_keys('gg0iX')
        input_driver.press('esc')
        type_keys('l.')
        save_and_check('insert-dot', ['XXone', 'one', 'three'])
        type_keys('gg0RAB')
        input_driver.press('esc')
        save_and_check('replace-mode', ['ABone', 'one', 'three'])
        type_keys(':%s/one/ONE/g')
        input_driver.press('enter')
        save_and_check('global-substitution', ['ABONE', 'ONE', 'three'])
        type_keys('u')
        save_and_check('substitution-undo', ['ABone', 'one', 'three'])
        input_driver.hotkey('ctrl', 'r')
        save_and_check('substitution-redo', ['ABONE', 'ONE', 'three'])
        type_keys('/three')
        input_driver.press('enter')
        type_keys('0rT')
        save_and_check('forward-search', ['ABONE', 'ONE', 'Three'])
        type_keys('?ABONE')
        input_driver.press('enter')
        type_keys('0rZ')
        save_and_check('backward-search', ['ZBONE', 'ONE', 'Three'])
        type_keys('gg0vld')
        save_and_check('visual-character', ['ONE', 'ONE', 'Three'])
        type_keys('u')
        save_and_check('visual-character-undo', ['ZBONE', 'ONE', 'Three'])
        type_keys('ggVd')
        save_and_check('visual-line', ['ONE', 'Three'])
        type_keys('u')
        save_and_check('visual-line-undo', ['ZBONE', 'ONE', 'Three'])
        type_keys('gg0')
        input_driver.hotkey('ctrl', 'v')
        type_keys('jld')
        save_and_check('visual-block', ['ONE', 'E', 'Three'])
        type_keys('u')
        save_and_check('visual-block-undo', ['ZBONE', 'ONE', 'Three'])
        (output / 'result.json').write_text(json.dumps({'status':'passed', 'input':'OS keyboard and mouse', 'default':'Vim'}))
    finally:
        try:
            gui.screenshot().save(output / 'desktop.png')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=15)
