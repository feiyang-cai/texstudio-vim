"""Fail-closed guards for disposable hosted Windows GUI tests.

The updater mitigation is deliberately opt-in and best effort. It never changes
settings, installs WSL, kills Terminal, or treats ancestry names as trusted. Its
approved boundary is the native system wsl.exe, exactly three updater arguments,
and this interactive session. Ancestry is diagnostic, not authorization.
"""
import ctypes
from ctypes import wintypes
import json
import ntpath
import os
from pathlib import Path
import subprocess
import threading
import time


class GuardError(RuntimeError):
    pass


def updater_candidate(record, argv, system_root, session):
    """Return true only for the exact updater; ambiguity is an error, not a match."""
    expected = ntpath.normcase(ntpath.join(system_root, 'System32', 'wsl.exe'))
    if not record.get('path') or not argv or record.get('session') is None or not record.get('created'):
        raise GuardError('WSL process identity is incomplete')
    arguments = argv[1:]
    if arguments != ['--update', '--confirm', '--prompt-before-exit']:
        # Ordinary WSL use must never be stopped. Near matches are not authorized.
        return False
    if (ntpath.normcase(record['path']) != expected or
            ntpath.normcase(argv[0]) not in (expected, 'wsl.exe') or record['session'] != session):
        raise GuardError('WSL updater path, command executable, or session is outside the allowed boundary')
    return True


def verify_held_identity(record, image, created, session, running):
    # CIM is microsecond precision, FILETIME is 100 ns. Any different timestamp
    # at CIM's precision is PID reuse or ambiguous provenance and must block.
    if (created // 10 != int(record['created']) // 10 or
            ntpath.normcase(image) != ntpath.normcase(record['path']) or
            session != record['session'] or not running):
        raise GuardError('Updater identity changed or exited before cleanup')


def startup_caption_focus(driver, pid, geometry, guard, hit_test=None):
    """One startup-only click, and only on a hit-tested caption owned by the app."""
    if guard.state.armed:
        raise GuardError('Startup focus recovery is forbidden after input is armed')
    if guard.owner() == pid:
        return
    x, y, width, _height = geometry
    point = (x + width // 2, y + 12)
    if hit_test is None:
        api = ctypes.WinDLL('user32', use_last_error=True)
        api.WindowFromPoint.argtypes = [wintypes.POINT]
        api.WindowFromPoint.restype = wintypes.HWND
        api.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        def hit_test(point):
            owner = wintypes.DWORD()
            window = api.WindowFromPoint(wintypes.POINT(*point))
            api.GetWindowThreadProcessId(window, ctypes.byref(owner))
            return owner.value
    if hit_test(point) != pid:
        raise GuardError('Application caption is obstructed by an unknown window; no input sent')
    driver.click(*point)
    guard.check()


def fence_event_matches(sentinel, nonce, window, event, object_id, child):
    return (event == 0x800C and window == sentinel and object_id == -4 and
            child == nonce and child > 0)


class GuardedInput:
    """Every input wrapper is guarded; recovery is forbidden once armed."""
    def __init__(self, driver, guard):
        self.driver, self.guard = driver, guard

    def _call(self, method, *args, **kwargs):
        self.guard.check()
        result = getattr(self.driver, method)(*args, **kwargs)
        self.guard.check()
        return result

    def write(self, text, interval=0):
        for char in text:
            self._call('write', char, interval=interval)

    def press(self, *args, **kwargs):
        return self._call('press', *args, **kwargs)

    def hotkey(self, *args, **kwargs):
        return self._call('hotkey', *args, **kwargs)

    def click(self, *args, **kwargs):
        return self._call('click', *args, **kwargs)


class FocusState:
    """Pure state machine: a transient focus loss can never be cleared/replayed."""
    def __init__(self, pid):
        self.pid, self.armed, self.fault = pid, False, None

    def fail(self, reason):
        if self.fault is None:
            self.fault = reason

    def event(self, owner):
        if self.armed and owner != self.pid:
            self.fail(f'Desktop focus changed during input: expected {self.pid}, got {owner}')

    def check(self, owner):
        self.event(owner)
        if self.fault:
            raise GuardError(self.fault)
        if owner != self.pid:
            raise GuardError(f'Desktop focus is not on the packaged application: {owner}')


class WindowsFocusGuard:
    """WinEvent hook with a dedicated message pump, plus synchronous PID checks."""
    def __init__(self, pid, output):
        self.state = FocusState(pid)
        self.monitor_fault = None
        self.foreground_events = []
        self.output = Path(output)
        self.ready, self.stopped = threading.Event(), threading.Event()
        self.drain = threading.Event()
        self.fence_lock = threading.Lock()
        self.fence_nonce = 0
        self.test_pause = None
        self.thread = threading.Thread(target=self._watch, daemon=True)
        self.thread.start()
        if not self.ready.wait(10):
            self.fail_monitor('Foreground monitor did not become ready')
        if self.state.fault:
            self.close()
            raise GuardError(self.state.fault)

    def fail_monitor(self, reason):
        if self.monitor_fault is None:
            self.monitor_fault = reason
        self.state.fail(reason)

    def owner(self):
        api = ctypes.WinDLL('user32', use_last_error=True)
        api.GetForegroundWindow.restype = wintypes.HWND
        api.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        pid = wintypes.DWORD()
        api.GetWindowThreadProcessId(api.GetForegroundWindow(), ctypes.byref(pid))
        return pid.value

    def _watch(self):
        api = ctypes.WinDLL('user32', use_last_error=True)
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        hook = None
        try:
            self.thread_id = kernel.GetCurrentThreadId()
            callback_type = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, wintypes.DWORD,
                                               wintypes.HWND, wintypes.LONG, wintypes.LONG,
                                               wintypes.DWORD, wintypes.DWORD)
            api.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
            _, self.sentinel = _test_window('TeXstudio CI foreground event fence', 'LISTBOX')
            @callback_type
            def callback(_hook, _event, window, _object, _child, _thread, _time):
                try:
                    if fence_event_matches(self.sentinel, self.fence_nonce, window, _event, _object, _child):
                        self.drain.set()
                        return
                    if _event != 3:
                        return
                    pid = wintypes.DWORD()
                    api.GetWindowThreadProcessId(window, ctypes.byref(pid))
                    # A destroyed/unresolvable event window produces PID 0 and
                    # is fatal after arm, even if cleanup already restored us.
                    self.foreground_events.append((window, pid.value))
                    self.state.event(pid.value)
                    with (self.output / 'foreground-events.jsonl').open('a', encoding='utf-8') as log:
                        log.write(json.dumps({'pid': pid.value, 'window': window,
                                              'armed': self.state.armed, 'event_time': _time}) + '\n')
                except Exception as error:
                    self.fail_monitor(f'Foreground callback failed: {error}')
            self.callback = callback
            api.SetWinEventHook.argtypes = [wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                           callback_type, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD]
            api.SetWinEventHook.restype = ctypes.c_void_p
            api.UnhookWinEvent.argtypes = [ctypes.c_void_p]
            message = wintypes.MSG()
            api.PeekMessageW(ctypes.byref(message), None, 0, 0, 0)
            # One ordered WinEvent stream: foreground events and our private
            # sentinel's name-change fence. Ignore all other events in the range.
            hook = api.SetWinEventHook(3, 0x800C, None, callback, 0, 0, 0)
            if not hook:
                raise GuardError(f'SetWinEventHook failed: {ctypes.get_last_error()}')
            self.ready.set()
            while True:
                result = api.GetMessageW(ctypes.byref(message), None, 0, 0)
                if result == -1:
                    raise GuardError(f'Foreground message pump failed: {ctypes.get_last_error()}')
                if result == 0:
                    break
                if message.message == 0x8002 and self.test_pause:
                    paused, resume = self.test_pause
                    paused.set()
                    if not resume.wait(10):
                        raise GuardError('Native self-test did not resume the foreground pump')
                else:
                    api.TranslateMessage(ctypes.byref(message))
                    api.DispatchMessageW(ctypes.byref(message))
        except Exception as error:
            self.fail_monitor(f'Foreground monitor failed: {error}')
        finally:
            if hook and not api.UnhookWinEvent(hook):
                self.fail_monitor('Could not stop foreground monitor')
            if hasattr(self, 'sentinel'):
                api.DestroyWindow.argtypes = [wintypes.HWND]
                api.DestroyWindow(self.sentinel)
            self.ready.set()
            self.stopped.set()

    def check(self):
        if self.stopped.is_set():
            self.fail_monitor('Foreground monitor stopped unexpectedly')
        self.flush()
        self.state.check(self.owner())

    def fence(self):
        """Fence the ordered WinEvent stream, even when a focus fault is latched.

        Each fence adds a uniquely numbered real item to a private hidden list
        box. Only its matching object/child name-change event acknowledges the
        fence. Ordinary, duplicate, and prior fence events cannot acknowledge a
        later fence. Serialize callers and keep the sentinel alive until unhook.
        """
        with self.fence_lock:
            if self.monitor_fault:
                raise GuardError(self.monitor_fault)
            self.drain.clear()
            api = ctypes.WinDLL('user32', use_last_error=True)
            api.SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, ctypes.c_size_t,
                                               ctypes.c_void_p, wintypes.UINT, wintypes.UINT,
                                               ctypes.POINTER(ctypes.c_size_t)]
            api.SendMessageTimeoutW.restype = ctypes.c_ssize_t
            name = ctypes.create_unicode_buffer(f'Fence {self.fence_nonce + 1}')
            # LB_ADDSTRING mutates the hidden list's actual accessibility state.
            result = ctypes.c_size_t()
            success = api.SendMessageTimeoutW(self.sentinel, 0x0180, 0, ctypes.cast(name, ctypes.c_void_p),
                                              0x0002, 5000, ctypes.byref(result))
            item = ctypes.c_ssize_t(result.value).value
            if not success or item < 0:
                self.fail_monitor('Foreground fence could not update its private sentinel')
                raise GuardError(self.monitor_fault)
            self.fence_nonce = item + 1  # List box accessibility children are 1-based.
            api.NotifyWinEvent.argtypes = [wintypes.DWORD, wintypes.HWND, wintypes.LONG, wintypes.LONG]
            api.NotifyWinEvent(0x800C, self.sentinel, -4, self.fence_nonce)
            if not self.drain.wait(5):
                self.fail_monitor('Foreground monitor did not acknowledge its WinEvent fence')
                raise GuardError('Foreground monitor did not acknowledge its WinEvent fence')
            if self.monitor_fault:
                raise GuardError(self.monitor_fault)

    def flush(self):
        self.fence()
        if self.state.fault:
            raise GuardError(self.state.fault)

    def arm(self):
        self.flush()
        self.check()
        self.state.armed = True
        self.check()

    def finish(self):
        self.flush()
        self.check()
        # Stay armed until shutdown; no restoration can erase a recorded loss.
        self.close()
        if self.state.fault:
            raise GuardError(self.state.fault)

    def close(self):
        if not self.stopped.is_set() and hasattr(self, 'thread_id'):
            api = ctypes.WinDLL('user32', use_last_error=True)
            if not api.PostThreadMessageW(self.thread_id, 0x0012, 0, 0):
                self.fail_monitor('Could not request foreground monitor shutdown')
            self.thread.join(5)
            if self.thread.is_alive():
                self.fail_monitor('Foreground monitor did not stop')


# One read-only CIM observer rather than a new PowerShell process per key. We
# collect ancestry for diagnosis, not to guess trusted provisioner filenames.
SNAPSHOTS = r'''
$ErrorActionPreference = 'Stop'
try {
  $subscription = Register-CimIndicationEvent -Query "SELECT * FROM Win32_ProcessStartTrace WHERE ProcessName = 'wsl.exe'" -SourceIdentifier TexstudioWslStarts
  while ($true) {
    $starts = @(Get-Event -SourceIdentifier TexstudioWslStarts -ErrorAction SilentlyContinue | ForEach-Object {
      $e = $_.SourceEventArgs.NewEvent
      @{pid=$e.ProcessID; session=$e.SessionID; time=$e.TIME_CREATED.ToString()}
      Remove-Event -EventIdentifier $_.EventIdentifier
    })
    $records = @(Get-CimInstance Win32_Process -Filter "Name = 'wsl.exe'" | ForEach-Object {
      $p = $_
      $ancestors = @()
      $parentId = $p.ParentProcessId
      for ($i = 0; $i -lt 3 -and $parentId; $i++) {
        $parent = Get-CimInstance Win32_Process -Filter "ProcessId = $parentId"
        if (!$parent) { break }
        $ancestors += @{pid=$parent.ProcessId; path=$parent.ExecutablePath; session=$parent.SessionId; created=$parent.CreationDate.ToUniversalTime().ToFileTimeUtc().ToString()}
        $parentId = $parent.ParentProcessId
      }
      @{pid=$p.ProcessId; path=$p.ExecutablePath; command=$p.CommandLine; session=$p.SessionId; created=$p.CreationDate.ToUniversalTime().ToFileTimeUtc().ToString(); ancestors=$ancestors}
    })
    ConvertTo-Json -InputObject @{processes=$records; starts=$starts} -Depth 6 -Compress
    Start-Sleep -Milliseconds 250
  }
} catch { Write-Error $_; exit 1 }
'''


class WindowsUpdaterGuard:
    """Best-effort cleanup, with the independent foreground guard always armed."""
    def __init__(self, output):
        if not (os.environ.get('GITHUB_ACTIONS') == 'true' and
                os.environ.get('RUNNER_ENVIRONMENT') == 'github-hosted' and
                os.environ.get('RUNNER_OS') == 'Windows' and
                os.environ.get('RUNNER_ARCH') == 'ARM64' and
                os.environ.get('TEXSTUDIO_HOSTED_WSL_GUARD') == '1'):
            raise GuardError('Updater guard requires an explicitly opted-in GitHub-hosted Windows ARM runner')
        self.output = Path(output)
        self.root = os.environ['SystemRoot']
        self.session = wintypes.DWORD()
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        if not kernel.ProcessIdToSessionId(os.getpid(), ctypes.byref(self.session)):
            raise GuardError('Cannot identify this interactive session')
        self.armed, self.fault = False, None
        self.closed = False
        self.lock = threading.Lock()
        self.ready, self.stop = threading.Event(), threading.Event()
        self.last_scan, self.last_activity = 0, time.monotonic()
        self.log = (self.output / 'wsl-updater-guard.jsonl').open('a', encoding='utf-8')
        self.stderr = (self.output / 'wsl-observer-errors.log').open('w', encoding='utf-8')
        powershell = ntpath.join(self.root, 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe')
        self.process = subprocess.Popen([powershell, '-NoLogo', '-NoProfile', '-NonInteractive', '-Command', SNAPSHOTS],
                                        stdout=subprocess.PIPE, stderr=self.stderr, text=True,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
        self.thread = threading.Thread(target=self._watch, daemon=True)
        self.thread.start()
        if not self.ready.wait(20):
            self.fault = 'Updater observer did not become ready'
        try:
            self.check()
        except Exception:
            self.close()
            raise

    def _argv(self, command):
        if not command:
            return []
        shell = ctypes.WinDLL('shell32', use_last_error=True)
        shell.CommandLineToArgvW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
        shell.CommandLineToArgvW.restype = ctypes.POINTER(wintypes.LPWSTR)
        count = ctypes.c_int()
        pointer = shell.CommandLineToArgvW(command, ctypes.byref(count))
        if not pointer:
            raise GuardError('Cannot parse WSL command line')
        try:
            return [pointer[i] for i in range(count.value)]
        finally:
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.LocalFree.argtypes = [ctypes.c_void_p]
            kernel.LocalFree(pointer)

    def _watch(self):
        try:
            for line in self.process.stdout:
                with self.lock:
                    if self.stop.is_set():
                        break
                    snapshot = json.loads(line)
                    # Start events are diagnostic only: a short-lived ordinary
                    # WSL probe (or our already-cleaned updater) may exit before
                    # inspection. Never terminate a process based on its name.
                    # The independent persistent foreground hook detects actual
                    # interference, including events from destroyed windows.
                    records = {record['pid']: record for record in snapshot['processes']}
                    for event in snapshot['starts']:
                        self.last_activity = time.monotonic()
                        self.log.write(json.dumps({'event': 'process-start', 'armed': self.armed, **event}) + '\n')
                    for record in records.values():
                        argv = self._argv(record.get('command'))
                        if updater_candidate(record, argv, self.root, self.session.value):
                            self.last_activity = time.monotonic()
                            self.log.write(json.dumps({'event': 'updater', 'armed': self.armed, **record}) + '\n')
                            self.log.flush()
                            self._terminate(record)
                    self.log.flush()
                self.last_scan = time.monotonic()
                self.ready.set()
            if not self.stop.is_set():
                raise GuardError('Updater observer exited unexpectedly')
        except Exception as error:
            self.fault = f'Updater guard failed: {error}'
            self.log.write(json.dumps({'event': 'failure', 'reason': self.fault}) + '\n')
            self.log.flush()
            self.ready.set()

    def _terminate(self, record):
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
        kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        # Keep this handle open from identity validation through termination. A
        # second lookup by PID is insufficient because the PID could be reused.
        handle = kernel.OpenProcess(0x1000 | 0x0001 | 0x00100000, False, record['pid'])
        if not handle:
            raise GuardError(f'Cannot open updater for verified cleanup: {ctypes.get_last_error()}')
        try:
            image = ctypes.create_unicode_buffer(32768)
            length = wintypes.DWORD(len(image))
            times = [wintypes.FILETIME() for _ in range(4)]
            session = wintypes.DWORD()
            if not (kernel.QueryFullProcessImageNameW(handle, 0, image, ctypes.byref(length)) and
                    kernel.GetProcessTimes(handle, *(ctypes.byref(t) for t in times)) and
                    kernel.ProcessIdToSessionId(record['pid'], ctypes.byref(session))):
                raise GuardError(f'Cannot verify updater identity: {ctypes.get_last_error()}')
            created = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
            verify_held_identity(record, image.value, created, session.value,
                                 kernel.WaitForSingleObject(handle, 0) == 258)
            if not kernel.TerminateProcess(handle, 1):
                raise GuardError(f'Updater cleanup denied: {ctypes.get_last_error()}')
            if kernel.WaitForSingleObject(handle, 5000) != 0:
                raise GuardError('Updater cleanup did not complete')
            self.log.write(json.dumps({'event': 'terminated', 'pid': record['pid'], 'created': record['created']}) + '\n')
            self.log.flush()
        finally:
            kernel.CloseHandle(handle)

    def check(self):
        if self.fault:
            raise GuardError(self.fault)
        if self.process.poll() is not None or not self.thread.is_alive():
            raise GuardError('Updater observer is not running')
        if time.monotonic() - self.last_scan > 10:
            raise GuardError('Updater observer heartbeat is stale')

    def flush(self):
        # Wait for a post-input process snapshot/event drain before accepting the
        # result. A missing observer heartbeat is a failure, never a soft skip.
        after = time.monotonic()
        deadline = after + 10
        while self.last_scan <= after:
            self.check()
            if time.monotonic() >= deadline:
                raise GuardError('Updater observer did not drain')
            time.sleep(.05)
        self.check()

    def quiet(self, seconds=5, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.check()
            if time.monotonic() - self.last_activity >= seconds:
                return
            time.sleep(.1)
        raise GuardError('Updater did not become quiet before startup deadline')

    def arm(self):
        self.check()
        with self.lock:
            self.armed = True
        self.check()

    def finish(self):
        self.flush()
        self.check()
        self.close()
        if self.fault:
            raise GuardError(self.fault)

    def close(self):
        if self.closed:
            return
        # Serialize shutdown with snapshot validation/cleanup. Once stopping,
        # ignore partial stdout from intentionally terminating our own observer.
        with self.lock:
            self.stop.set()
        if self.process.poll() is None:
            self.process.terminate()
        self.thread.join(10)
        self.process.wait(timeout=10)
        if self.thread.is_alive():
            raise GuardError('Updater observer did not stop')
        self.log.close()
        self.stderr.close()
        self.closed = True


def _test_window(title, window_class='STATIC'):
    """Create only our own disposable native window; do not type into any window."""
    api = ctypes.WinDLL('user32', use_last_error=True)
    api.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                   wintypes.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, wintypes.HWND, ctypes.c_void_p,
                                   ctypes.c_void_p, ctypes.c_void_p]
    api.CreateWindowExW.restype = wintypes.HWND
    api.DestroyWindow.argtypes = [wintypes.HWND]
    window = api.CreateWindowExW(0, window_class, title, 0x00CF0000, 50, 50, 300, 120,
                                None, None, None, None)
    if not window:
        raise GuardError('Could not create native focus self-test window')
    return api, window


def pump_messages(api):
    message = wintypes.MSG()
    while api.PeekMessageW(ctypes.byref(message), None, 0, 0, 1):
        api.TranslateMessage(ctypes.byref(message))
        api.DispatchMessageW(ctypes.byref(message))


def verify_probe_window(api, child, probe):
    """The handshake must name a live window of this exact owned subprocess."""
    window = probe.get('window')
    if (child.poll() is not None or probe.get('pid') != child.pid or
            not isinstance(window, int) or window <= 0):
        raise GuardError('Focus probe exited or returned an invalid window identity')
    api.IsWindow.argtypes = [wintypes.HWND]
    api.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    owner = wintypes.DWORD()
    thread = api.GetWindowThreadProcessId(window, ctypes.byref(owner))
    if not thread or not api.IsWindow(window) or owner.value != child.pid:
        raise GuardError('Focus probe HWND does not belong to the live owned subprocess')
    return window


def wait_probe_ready(api, child, probe_path, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if child.poll() is not None:
            raise GuardError(f'Focus probe exited before GUI readiness: {child.returncode}')
        if probe_path.exists():
            probe = json.loads(probe_path.read_text(encoding='utf-8'))
            verify_probe_window(api, child, probe)
            return probe
        pump_messages(api)
        time.sleep(.01)
    raise GuardError('Focus probe did not report GUI readiness before the deadline')


def focus_self_test(output):
    """Prove native callbacks detect an away-and-back transition before testing.

    No keyboard input, registry edits, installation, or unrelated process cleanup.
    Failure blocks the smoke test. This tests the actual hosted desktop's hook,
    not just the pure Python state machine.
    """
    import sys
    from windows_desktop_input import WindowsDesktopInput
    api, window = _test_window('TeXstudio CI focus monitor self-test')
    child = guard = None
    try:
        driver = WindowsDesktopInput()
        driver.activate(window)
        guard = WindowsFocusGuard(os.getpid(), output)
        # Foreground lock may reject activation even on an otherwise healthy
        # desktop. A self-owned hit-tested caption is the only click permitted.
        startup_caption_focus(driver, os.getpid(), (50, 50, 300, 120), guard)
        guard.arm()
        # The child initializes a real HWND and pumps its GUI queue before the
        # parent attempts any focus transfer. Popen alone is not GUI readiness.
        # The foreground parent activates that verified owned HWND directly;
        # no AllowSetForegroundWindow permission handoff is needed.
        import uuid
        probe_path = Path(output) / f'focus-probe-{uuid.uuid4().hex}.json'
        with (Path(output) / 'focus-probe-errors.log').open('w', encoding='utf-8') as errors:
            child = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '--focus-probe', str(probe_path)],
                                     stdout=subprocess.DEVNULL, stderr=errors,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
        probe = wait_probe_ready(api, child, probe_path)
        guard.check()
        paused, resume = threading.Event(), threading.Event()
        guard.test_pause = (paused, resume)
        if not api.PostThreadMessageW(guard.thread_id, 0x8002, 0, 0) or not paused.wait(5):
            raise GuardError('Could not pause the native self-test event pump')
        probe_window = verify_probe_window(api, child, probe)
        driver.activate(probe_window)
        probe_owner = guard.owner()
        (Path(output) / 'focus-probe-activation.json').write_text(json.dumps({
            'expected_pid': child.pid, 'foreground_pid': probe_owner,
            'window': probe_window, 'activation': driver.activation}), encoding='utf-8')
        if probe_owner != child.pid:
            raise GuardError('Foreground parent could not activate the verified probe HWND; see focus-probe-activation.json')
        until = time.monotonic() + .3
        while time.monotonic() < until:
            pump_messages(api)
            time.sleep(.01)
        # Close only our exact verified probe, never a title-matched foreground
        # window. Keep the observer paused until this source HWND is destroyed.
        verify_probe_window(api, child, probe)
        api.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t]
        ctypes.set_last_error(0)
        if not api.PostMessageW(probe_window, 0x0010, 0, 0):
            raise GuardError(f'Could not close the owned focus probe: {ctypes.get_last_error()}')
        deadline = time.monotonic() + 5
        while child.poll() is None and time.monotonic() < deadline:
            pump_messages(api)
            time.sleep(.01)
        if child.poll() is None:
            raise GuardError('Native focus probe did not exit after its verified close request')
        if child.returncode:
            raise GuardError(f'Native focus probe failed: {child.returncode}; see focus-probe-errors.log')
        driver.activate(window)
        resume.set()
        guard.fence()
        if guard.owner() != os.getpid():
            raise GuardError('Focus self-test could not restore its own window')
        if not any(window == probe['window'] and pid in (0, child.pid) for window, pid in guard.foreground_events):
            raise GuardError('Native foreground hook missed the known probe window')
        if not guard.state.fault or 'Desktop focus changed' not in guard.state.fault:
            raise GuardError('Native foreground hook missed an away-and-back transition')
        guard.close()
        if guard.monitor_fault:
            raise GuardError(guard.monitor_fault)
        (Path(output) / 'focus-monitor-self-test.json').write_text(
            json.dumps({'status': 'passed', 'detected': guard.state.fault}), encoding='utf-8')
    finally:
        if child and child.poll() is None:
            child.kill()
            child.wait(timeout=5)
        if guard:
            if guard.test_pause:
                guard.test_pause[1].set()
            guard.close()
        api.DestroyWindow(window)


if __name__ == '__main__':
    import sys
    if len(sys.argv) != 3 or sys.argv[1] != '--focus-probe' or sys.platform != 'win32':
        raise SystemExit('Only the disposable Windows foreground probe is supported')
    api, window = _test_window('TeXstudio CI temporary focus obstruction')
    try:
        # Atomic file publication is the readiness handshake. The window starts
        # hidden and this process continuously pumps messages; only the verified
        # foreground parent may activate and close it.
        probe_path = Path(sys.argv[2])
        temporary = probe_path.with_suffix('.tmp')
        temporary.write_text(json.dumps({'pid': os.getpid(), 'window': window}), encoding='utf-8')
        temporary.replace(probe_path)
        api.IsWindow.argtypes = [wintypes.HWND]
        deadline = time.monotonic() + 30
        while api.IsWindow(window) and time.monotonic() < deadline:
            pump_messages(api)
            time.sleep(.01)
        if api.IsWindow(window):
            raise GuardError('Focus probe timed out without its verified close request')
    finally:
        if api.IsWindow(window):
            api.DestroyWindow(window)
