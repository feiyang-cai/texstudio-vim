"""Fail-closed safety tests; real WinEvent behavior also requires the native probe."""
import ast
import ctypes
import importlib.util
import io
from pathlib import Path
import sys
import threading
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
from windows_gui_guard import (FocusState, GuardError, GuardedInput,
                               WindowsFocusGuard, WindowsUpdaterGuard,
                               startup_caption_focus, updater_candidate,
                               verify_held_identity, fence_event_matches, verify_probe_window, wait_probe_ready)
from windows_desktop_input import WindowsDesktopInput

ROOT = r'C:\Windows'
IMAGE = ROOT + r'\System32\wsl.exe'
ARGS = [IMAGE, '--update', '--confirm', '--prompt-before-exit']
RECORD = {'pid': 123, 'path': IMAGE, 'session': 1, 'created': '123456780'}


class UpdaterMatchTests(unittest.TestCase):
    def test_exact_updater(self):
        self.assertTrue(updater_candidate(RECORD, ARGS, ROOT, 1))

    def test_case_insensitive_windows_image(self):
        self.assertTrue(updater_candidate(dict(RECORD, path=IMAGE.upper()),
                                         [IMAGE.upper()] + ARGS[1:], ROOT, 1))

    def test_bare_argv_executable_still_requires_verified_system_image(self):
        self.assertTrue(updater_candidate(RECORD, ['wsl.exe'] + ARGS[1:], ROOT, 1))
        self.assertTrue(updater_candidate(RECORD, ['WSL.EXE'] + ARGS[1:], ROOT, 1))
        with self.assertRaises(GuardError):
            updater_candidate(dict(RECORD, path=r'C:\Temp\wsl.exe'), ['wsl.exe'] + ARGS[1:], ROOT, 1)

    def test_extra_arguments_are_not_authorized(self):
        self.assertFalse(updater_candidate(RECORD, ARGS + ['--web-download'], ROOT, 1))

    def test_reordered_arguments_are_not_authorized(self):
        self.assertFalse(updater_candidate(RECORD, [IMAGE] + ARGS[:0:-1], ROOT, 1))

    def test_ordinary_wsl_commands_are_never_targets(self):
        for args in (['--status'], ['--list'], ['--exec', 'echo', 'hi'], [], ['--update']):
            with self.subTest(args=args):
                self.assertFalse(updater_candidate(RECORD, [IMAGE] + args, ROOT, 1))

    def test_missing_identity_fails_closed(self):
        for field in ('path', 'session', 'created'):
            with self.subTest(field=field), self.assertRaises(GuardError):
                updater_candidate(dict(RECORD, **{field: None}), ARGS, ROOT, 1)
        with self.assertRaises(GuardError):
            updater_candidate(RECORD, [], ROOT, 1)

    def test_wrong_path_or_command_executable_fails_closed(self):
        for path in (r'C:\Temp\wsl.exe', r'C:\Windows\SysWOW64\wsl.exe', r'.\wsl.exe', 'alias.exe'):
            with self.subTest(path=path), self.assertRaises(GuardError):
                updater_candidate(dict(RECORD, path=path), ARGS, ROOT, 1)
            with self.subTest(argv=path), self.assertRaises(GuardError):
                updater_candidate(RECORD, [path] + ARGS[1:], ROOT, 1)

    def test_other_session_fails_closed(self):
        with self.assertRaises(GuardError):
            updater_candidate(RECORD, ARGS, ROOT, 2)

    def test_ancestry_is_not_invented_as_authorization(self):
        self.assertTrue(updater_candidate(dict(RECORD, ancestors=[]), ARGS, ROOT, 1))

    def test_held_identity(self):
        verify_held_identity(RECORD, IMAGE, 123456789, 1, True)

    def test_pid_reuse_fails_closed(self):
        with self.assertRaises(GuardError):
            verify_held_identity(RECORD, IMAGE, 123456790, 1, True)

    def test_changed_image_or_session_or_exit_fails_closed(self):
        for image, session, running in ((r'C:\Temp\wsl.exe', 1, True), (IMAGE, 2, True), (IMAGE, 1, False)):
            with self.subTest(image=image, session=session, running=running), self.assertRaises(GuardError):
                verify_held_identity(RECORD, image, 123456780, session, running)

    def test_not_opted_in_never_starts_observer(self):
        with patch.dict('os.environ', {}, clear=True), patch('subprocess.Popen') as popen:
            with self.assertRaises(GuardError):
                WindowsUpdaterGuard('.')
            popen.assert_not_called()


class FocusTests(unittest.TestCase):
    def state(self):
        state = FocusState(42)
        state.armed = True
        return state

    def test_expected_owner(self):
        self.state().check(42)

    def test_unknown_owner_fatal(self):
        with self.assertRaises(GuardError):
            self.state().check(0)

    def test_leave_and_return_stays_fatal(self):
        state = self.state()
        state.event(99)
        state.event(42)
        with self.assertRaises(GuardError):
            state.check(42)

    def test_destroyed_window_owner_stays_fatal(self):
        state = self.state()
        state.event(0)
        with self.assertRaises(GuardError):
            state.check(42)

    def test_monitor_failure_stays_fatal(self):
        state = self.state()
        state.fail('observer exited')
        state.event(42)
        with self.assertRaisesRegex(GuardError, 'observer exited'):
            state.check(42)

    def test_initial_obstruction_does_not_send_keys(self):
        driver = Mock()
        guard = Mock(state=FocusState(42))
        guard.owner.return_value = 99
        with self.assertRaisesRegex(GuardError, 'obstructed'):
            startup_caption_focus(driver, 42, (0, 0, 800, 600), guard, lambda point: 99)
        driver.click.assert_not_called()

    def test_startup_click_requires_target_caption(self):
        driver = Mock()
        guard = Mock(state=FocusState(42))
        guard.owner.return_value = 99
        startup_caption_focus(driver, 42, (0, 0, 800, 600), guard, lambda point: 42)
        driver.click.assert_called_once_with(400, 12)
        guard.check.assert_called_once()

    def test_no_startup_recovery_after_arming(self):
        driver = Mock()
        guard = Mock(state=self.state())
        with self.assertRaises(GuardError):
            startup_caption_focus(driver, 42, (0, 0, 800, 600), guard, lambda point: 42)
        driver.click.assert_not_called()

    def test_all_wrappers_prevent_input_with_wrong_focus(self):
        for method, args in [('write', ('x',)), ('press', ('enter',)),
                             ('hotkey', ('ctrl', 'r')), ('click', (2, 3))]:
            with self.subTest(method=method):
                driver = Mock()
                guard = Mock()
                guard.check.side_effect = GuardError('wrong focus')
                with self.assertRaises(GuardError):
                    getattr(GuardedInput(driver, guard), method)(*args)
                getattr(driver, method).assert_not_called()

    def test_all_wrappers_postcheck_and_never_replay(self):
        for method, args in [('write', ('x',)), ('press', ('enter',)),
                             ('hotkey', ('ctrl', 'r')), ('click', (2, 3))]:
            with self.subTest(method=method):
                driver, guard = Mock(), Mock()
                guard.check.side_effect = [None, GuardError('lost focus')]
                with self.assertRaises(GuardError):
                    getattr(GuardedInput(driver, guard), method)(*args)
                self.assertEqual(getattr(driver, method).call_count, 1)

    def test_mid_text_theft_sends_no_next_character(self):
        driver, guard = Mock(), Mock()
        guard.check.side_effect = [None, None, GuardError('lost focus')]
        with self.assertRaises(GuardError):
            GuardedInput(driver, guard).write(':w')
        driver.write.assert_called_once_with(':', interval=0)

    def test_native_sendinput_has_pre_and_post_guards(self):
        driver = object.__new__(WindowsDesktopInput)
        driver.api = Mock()
        driver.api.SendInput.return_value = 1
        driver.input_guard = Mock(side_effect=[None, GuardError('lost focus')])
        with self.assertRaises(GuardError):
            driver.send(ctypes.c_int(0))
        self.assertEqual(driver.api.SendInput.call_count, 1)
        self.assertEqual(driver.input_guard.call_count, 2)

    def test_native_sendinput_blocked_before_os_call(self):
        driver = object.__new__(WindowsDesktopInput)
        driver.api = Mock()
        driver.input_guard = Mock(side_effect=GuardError('lost focus'))
        with self.assertRaises(GuardError):
            driver.send(ctypes.c_int(0))
        driver.api.SendInput.assert_not_called()

    def test_late_event_before_finish_prevents_pass(self):
        guard = object.__new__(WindowsFocusGuard)
        guard.state = self.state()
        guard.monitor_fault = None
        guard.flush = lambda: guard.state.event(99)
        guard.check = lambda: guard.state.check(42)
        guard.close = Mock()
        with self.assertRaises(GuardError):
            guard.finish()
        guard.close.assert_not_called()

    def test_stopped_monitor_fails_closed(self):
        guard = object.__new__(WindowsFocusGuard)
        guard.state = self.state()
        guard.monitor_fault = None
        guard.stopped = threading.Event()
        guard.stopped.set()
        guard.flush = Mock()
        guard.owner = lambda: 42
        with self.assertRaises(GuardError):
            guard.check()


class ProbeHandshakeTests(unittest.TestCase):
    def native(self, owner=123, alive=True):
        api = Mock()
        api.IsWindow.return_value = alive
        api.GetWindowThreadProcessId.side_effect = lambda window, pid: setattr(pid._obj, 'value', owner) or 4
        child = Mock(pid=123, returncode=None)
        child.poll.return_value = None
        return api, child

    def test_ready_window_must_belong_to_exact_owned_child(self):
        api, child = self.native()
        self.assertEqual(verify_probe_window(api, child, {'pid': 123, 'window': 456}), 456)

    def test_exited_child_is_rejected_before_any_window_action(self):
        api, child = self.native()
        child.poll.return_value = 1
        with self.assertRaises(GuardError):
            verify_probe_window(api, child, {'pid': 123, 'window': 456})
        api.IsWindow.assert_not_called()

    def test_handshake_other_pid_is_rejected(self):
        api, child = self.native()
        with self.assertRaises(GuardError):
            verify_probe_window(api, child, {'pid': 999, 'window': 456})

    def test_reused_hwnd_other_owner_is_rejected(self):
        api, child = self.native(owner=999)
        with self.assertRaises(GuardError):
            verify_probe_window(api, child, {'pid': 123, 'window': 456})

    def test_destroyed_hwnd_is_rejected(self):
        api, child = self.native(alive=False)
        with self.assertRaises(GuardError):
            verify_probe_window(api, child, {'pid': 123, 'window': 456})

    def test_missing_or_invalid_hwnd_is_rejected(self):
        for window in (None, 0, -1, '456'):
            api, child = self.native()
            with self.subTest(window=window), self.assertRaises(GuardError):
                verify_probe_window(api, child, {'pid': 123, 'window': window})

    def test_readiness_accepts_only_published_verified_window(self):
        api, child = self.native()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'ready.json'
            path.write_text('{"pid":123,"window":456}', encoding='utf-8')
            self.assertEqual(wait_probe_ready(api, child, path), {'pid': 123, 'window': 456})

    def test_child_exit_before_ready_is_fatal(self):
        api, child = self.native()
        child.returncode = 7
        child.poll.return_value = 7
        with self.assertRaisesRegex(GuardError, 'before GUI readiness: 7'):
            wait_probe_ready(api, child, Path('missing-probe-record'))

    def test_missing_readiness_times_out_without_window_actions(self):
        api, child = self.native()
        with self.assertRaisesRegex(GuardError, 'deadline'):
            wait_probe_ready(api, child, Path('missing-probe-record'), timeout=0)
        api.IsWindow.assert_not_called()

    def test_self_test_does_not_depend_on_foreground_permission_handoff(self):
        source = (SCRIPTS / 'windows_gui_guard.py').read_text()
        self.assertNotIn('api.AllowSetForegroundWindow(', source)
        self.assertNotIn('sys.stdin.readline()', source)
        self.assertLess(source.index('probe = wait_probe_ready('), source.index('driver.activate(probe_window)'))
        self.assertIn('probe_window = verify_probe_window(api, child, probe)', source)
        self.assertIn('api.PostMessageW(probe_window, 0x0010, 0, 0)', source)


class NativeDiagnosticsTests(unittest.TestCase):
    def test_activation_success_does_not_report_stale_error(self):
        self.activation_error_case(True, 0)

    def test_activation_failure_retains_real_error(self):
        self.activation_error_case(False, 5)

    def activation_error_case(self, success, error):
        driver = object.__new__(WindowsDesktopInput)
        driver.api = Mock()
        driver.api.GetWindowThreadProcessId.side_effect = [1, 2]
        driver.api.AttachThreadInput.return_value = success
        kernel = Mock()
        kernel.GetCurrentThreadId.return_value = 3
        with patch.object(ctypes, 'windll', SimpleNamespace(kernel32=kernel), create=True), \
                patch.object(ctypes, 'set_last_error', create=True), \
                patch.object(ctypes, 'get_last_error', return_value=5, create=True):
            driver.activate(11)
        self.assertEqual(len(driver.activation['attachments']), 2)
        self.assertTrue(all(item['error'] == error for item in driver.activation['attachments']))


class FenceTests(unittest.TestCase):
    def test_current_unique_fence(self):
        self.assertTrue(fence_event_matches(100, 2, 100, 0x800C, -4, 2))

    def test_old_duplicate_fence_cannot_acknowledge_next(self):
        self.assertFalse(fence_event_matches(100, 2, 100, 0x800C, -4, 1))

    def test_ordinary_name_change_cannot_acknowledge(self):
        self.assertFalse(fence_event_matches(100, 2, 100, 0x800C, -4, 0))

    def test_other_window_or_object_cannot_acknowledge(self):
        self.assertFalse(fence_event_matches(100, 2, 101, 0x800C, -4, 2))
        self.assertFalse(fence_event_matches(100, 2, 100, 0x800C, 0, 2))
        self.assertFalse(fence_event_matches(100, 2, 100, 3, -4, 2))

    def test_health_failure_not_masked_by_expected_probe_focus_loss(self):
        guard = object.__new__(WindowsFocusGuard)
        guard.state = FocusState(42)
        guard.state.armed = True
        guard.state.event(99)
        guard.monitor_fault = None
        guard.fail_monitor('fence timeout')
        self.assertIn('Desktop focus changed', guard.state.fault)
        self.assertEqual(guard.monitor_fault, 'fence timeout')
        guard.fence_lock = threading.Lock()
        with self.assertRaisesRegex(GuardError, 'fence timeout'):
            guard.fence()


class CleanupTests(unittest.TestCase):
    def fake_kernel(self):
        kernel = Mock()
        kernel.OpenProcess.return_value = 7
        kernel.QueryFullProcessImageNameW.side_effect = lambda handle, flags, image, length: setattr(image, 'value', IMAGE) or 1
        def times(handle, created, *others):
            created._obj.dwLowDateTime = 123456780
            created._obj.dwHighDateTime = 0
            return 1
        kernel.GetProcessTimes.side_effect = times
        kernel.ProcessIdToSessionId.side_effect = lambda pid, session: setattr(session._obj, 'value', 1) or 1
        kernel.WaitForSingleObject.side_effect = [258, 0]
        kernel.TerminateProcess.return_value = 1
        return kernel

    def run_cleanup(self, kernel):
        guard = object.__new__(WindowsUpdaterGuard)
        guard.session = Mock(value=1)
        guard.log = io.StringIO()
        with patch.object(ctypes, 'WinDLL', return_value=kernel, create=True), \
                patch.object(ctypes, 'get_last_error', return_value=5, create=True):
            guard._terminate(RECORD)
        return guard

    def test_termination_uses_same_held_handle_and_closes_it(self):
        kernel = self.fake_kernel()
        self.run_cleanup(kernel)
        kernel.TerminateProcess.assert_called_once_with(7, 1)
        kernel.CloseHandle.assert_called_once_with(7)

    def test_denied_open_never_terminates(self):
        kernel = self.fake_kernel()
        kernel.OpenProcess.return_value = 0
        with self.assertRaisesRegex(GuardError, 'Cannot open'):
            self.run_cleanup(kernel)
        kernel.TerminateProcess.assert_not_called()

    def test_denied_termination_is_fatal_and_handle_closed(self):
        kernel = self.fake_kernel()
        kernel.TerminateProcess.return_value = 0
        with self.assertRaisesRegex(GuardError, 'cleanup denied'):
            self.run_cleanup(kernel)
        kernel.CloseHandle.assert_called_once_with(7)

    def test_changed_identity_never_terminates(self):
        kernel = self.fake_kernel()
        kernel.ProcessIdToSessionId.side_effect = lambda pid, session: setattr(session._obj, 'value', 2) or 1
        with self.assertRaises(GuardError):
            self.run_cleanup(kernel)
        kernel.TerminateProcess.assert_not_called()
        kernel.CloseHandle.assert_called_once_with(7)

    def test_intentional_stop_ignores_partial_observer_line(self):
        guard = object.__new__(WindowsUpdaterGuard)
        guard.process = Mock(stdout=['{partial'])
        guard.stop = threading.Event()
        guard.stop.set()
        guard.lock = threading.Lock()
        guard.fault = None
        guard._watch()
        self.assertIsNone(guard.fault)

    def test_partial_observer_line_before_stop_is_fatal(self):
        guard = object.__new__(WindowsUpdaterGuard)
        guard.process = Mock(stdout=['{partial'])
        guard.stop = threading.Event()
        guard.lock = threading.Lock()
        guard.ready = threading.Event()
        guard.log = io.StringIO()
        guard.fault = None
        guard._watch()
        self.assertIsNotNone(guard.fault)
        self.assertTrue(guard.ready.is_set())

    def test_observer_fault_cannot_be_ignored(self):
        guard = object.__new__(WindowsUpdaterGuard)
        guard.fault = 'access denied'
        with self.assertRaises(GuardError):
            guard.check()

    def test_stale_observer_cannot_pass(self):
        guard = object.__new__(WindowsUpdaterGuard)
        guard.fault = None
        guard.process = Mock()
        guard.process.poll.return_value = None
        guard.thread = Mock()
        guard.thread.is_alive.return_value = True
        guard.last_scan = time.monotonic() - 11
        with self.assertRaisesRegex(GuardError, 'stale'):
            guard.check()


class IntegrationTests(unittest.TestCase):
    def test_smoke_never_closes_terminal_by_title(self):
        source = (SCRIPTS / 'smoke-native-package.py').read_text()
        self.assertNotIn('PostMessageW', source)
        self.assertNotIn('recover=', source)
        self.assertLess(source.index('focus_guard.arm()'), source.index("input_driver.press('esc')"))
        self.assertLess(source.index('focus_guard.finish()'), source.index("'status':'passed'"))

    def test_winevent_fence_is_not_posted_message_heartbeat(self):
        source = (SCRIPTS / 'windows_gui_guard.py').read_text()
        self.assertIn('api.NotifyWinEvent(0x800C, self.sentinel, -4, self.fence_nonce)', source)
        self.assertIn('api.SetWinEventHook(3, 0x800C', source)
        self.assertNotIn('PostThreadMessageW(self.thread_id, 0x8001', source)

    def test_ancestor_command_lines_are_not_collected(self):
        source = (SCRIPTS / 'windows_gui_guard.py').read_text()
        self.assertNotIn('$parent.CommandLine', source)

    def test_observer_shutdown_precedes_pass(self):
        source = (SCRIPTS / 'smoke-native-package.py').read_text()
        self.assertLess(source.index('updater_guard.finish()'), source.index('focus_guard.finish()'))
        self.assertLess(source.index('focus_guard.finish()'), source.index("'status':'passed'"))

    def test_guard_opt_in_is_only_windows_arm_in_both_workflows(self):
        for name in ('cd.yml', 'packaged-gui.yml'):
            source = (SCRIPTS.parent / 'workflows' / name).read_text()
            self.assertIn("TEXSTUDIO_HOSTED_WSL_GUARD: ${{ matrix.runner == 'windows-11-arm' && '1' || '0' }}", source)


if __name__ == '__main__':
    unittest.main()
