"""Mocked IFEO contract tests. These do not prove native transaction semantics."""
from contextlib import contextmanager
import copy
import ctypes
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
import windows_wsl_ifeo as module
from windows_wsl_ifeo import (BASE, BUILD_RUN, BUILD_SHA, CANONICAL, NativeRegistry,
                              WorkaroundError, approved_command, cleanup, context,
                              expected_data, setup)

COMMAND = r'"C:\Windows\System32\systray.exe"'
ENV = {'GITHUB_ACTIONS': 'true', 'RUNNER_ENVIRONMENT': 'github-hosted',
       'RUNNER_OS': 'Windows', 'RUNNER_ARCH': 'ARM64', 'GITHUB_EVENT_NAME': 'workflow_dispatch',
       'GITHUB_REPOSITORY': 'feiyang-cai/texstudio-vim',
       'GITHUB_WORKFLOW': 'Packaged desktop retest', 'GITHUB_JOB': 'desktop',
       'GITHUB_RUN_ATTEMPT': '1', 'TEXSTUDIO_TEMPORARY_WSL_IFEO': '1',
       'TEXSTUDIO_BUILD_RUN_ID': BUILD_RUN, 'BUILD_SHA': BUILD_SHA,
       'GITHUB_SHA': 'a' * 40, 'GITHUB_RUN_ID': '12345', 'TEXSTUDIO_IFEO_APPROVED_HEAD': 'a' * 40}
BINDING = context(ENV, 'win32')


class FakeRegistry:
    """Conflict injection exercises our contract, not Windows isolation itself."""
    def __init__(self):
        self.live = None
        self.pending = None
        self.active = False
        self.sets = self.deletes = self.commits = 0
        self.conflict = False
        self.fail_commit = False
        self.deny_set = False
        self.deny_delete = False
        self.unsupported = False

    @contextmanager
    def transaction(self):
        if self.unsupported:
            raise WorkaroundError('Unsupported transactions')
        assert not self.active
        self.active = True
        self.pending = copy.deepcopy(self.live)
        try:
            yield self
        finally:
            self.active = False
            self.pending = None

    def commit(self, transaction):
        assert self.active and transaction is self
        if self.conflict:
            self.live = {'foreign': 'preserve concurrent writer'}
            raise WorkaroundError('Transaction conflict')
        if self.fail_commit:
            raise WorkaroundError('Commit failed')
        self.live = copy.deepcopy(self.pending)
        self.commits += 1

    @contextmanager
    def parent(self, transaction):
        assert self.active and transaction is self
        yield 'parent'

    @contextmanager
    def create_new(self, parent, transaction):
        assert self.active and transaction is self and parent == 'parent'
        if self.pending is not None:
            raise WorkaroundError('Preexisting key')
        self.pending = {'values': 0, 'subkeys': 0, 'class_size': 0, 'security_size': 88,
                        'last_write': 10, 'canonical': CANONICAL.lower()}
        yield 'transacted'

    @contextmanager
    def open_target(self, transaction=None):
        key = 'transacted' if transaction is not None else 'live'
        if self.state(key) is None:
            raise WorkaroundError('Missing key')
        yield key

    def state(self, key):
        return self.pending if key == 'transacted' else self.live

    def set_debugger(self, key, command):
        assert self.active and key == 'transacted'
        if self.deny_set:
            raise WorkaroundError('Set denied')
        self.pending.update(values=1, command=command, last_write=20)
        self.sets += 1

    def snapshot(self, key, command):
        state = self.state(key)
        if (state.get('command') != command or state.get('values') != 1 or
                state.get('subkeys') != 0 or state.get('class_size') != 0):
            raise WorkaroundError('Unexpected exact state')
        return copy.deepcopy(state)

    def delete_debugger(self, key):
        assert self.active and key == 'transacted'
        if self.deny_delete:
            raise WorkaroundError('Delete denied')
        del self.pending['command']
        self.pending.update(values=0, last_write=30)
        self.deletes += 1

    def require_empty(self, key):
        state = self.state(key)
        if state['values'] or state['subkeys'] or state['class_size']:
            raise WorkaroundError('Not empty')


class GatingTests(unittest.TestCase):
    def test_exact_context_binds_head_run_attempt_original_source(self):
        self.assertEqual(BINDING, {'repository': ENV['GITHUB_REPOSITORY'], 'run_id': '12345',
                                  'attempt': '1', 'head': 'a' * 40,
                                  'build_run': BUILD_RUN, 'build_sha': BUILD_SHA})

    def test_missing_any_required_environment_is_rejected(self):
        for key in ENV:
            with self.subTest(key=key), self.assertRaises(WorkaroundError):
                context({k: v for k, v in ENV.items() if k != key}, 'win32')

    def test_nonmanual_nonhosted_retries_wrong_head_or_packages_rejected(self):
        for change in ({'GITHUB_EVENT_NAME': 'push'}, {'GITHUB_EVENT_NAME': 'workflow_call'},
                       {'GITHUB_EVENT_NAME': 'pull_request'}, {'RUNNER_ENVIRONMENT': 'self-hosted'},
                       {'RUNNER_ARCH': 'X64'}, {'GITHUB_RUN_ATTEMPT': '2'},
                       {'TEXSTUDIO_TEMPORARY_WSL_IFEO': '0'}, {'GITHUB_RUN_ID': '../other'},
                       {'GITHUB_SHA': 'bad'}, {'TEXSTUDIO_IFEO_APPROVED_HEAD': 'b' * 40},
                       {'BUILD_SHA': 'b' * 40}, {'TEXSTUDIO_BUILD_RUN_ID': '999'}):
            with self.subTest(change=change), self.assertRaises(WorkaroundError):
                context(dict(ENV, **change), 'win32')
        with self.assertRaises(WorkaroundError):
            context(ENV, 'linux')

    def test_builtin_command_only(self):
        system = {'python_bits': 64, 'systray': {'path': r'C:\Windows\System32\systray.exe', 'is_file': True}}
        self.assertEqual(approved_command(system), COMMAND)
        for path in (r'C:\Windows\SysWOW64\systray.exe', r'C:\Windows\System32\cmd.exe',
                     r'C:\Windows\System32\systray.exe" --extra', 'relative.exe'):
            with self.subTest(path=path), self.assertRaises(WorkaroundError):
                approved_command({'python_bits': 64, 'systray': {'path': path, 'is_file': True}})
        for bits, exists in ((32, True), (64, False)):
            with self.assertRaises(WorkaroundError):
                approved_command({'python_bits': bits, 'systray': dict(system['systray'], is_file=exists)})


class LifecycleTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'state.json'
        self.registry = FakeRegistry()

    def install(self):
        return setup(self.registry, BINDING, COMMAND, self.path)

    def clean(self):
        return cleanup(self.registry, BINDING, COMMAND, self.path)

    def test_single_setup_and_cleanup_leave_empty_shell(self):
        self.assertEqual(self.install()['phase'], 'installed')
        record = self.clean()
        self.assertEqual(record['phase'], 'cleaned')
        self.assertTrue(record['empty_key_left'])
        self.assertIsNotNone(self.registry.live)
        self.assertEqual(self.registry.live['values'], 0)
        self.assertEqual((self.registry.sets, self.registry.deletes, self.registry.commits), (1, 1, 2))

    def test_preexisting_empty_or_configured_key_never_adopted(self):
        for existing in ({}, {'command': COMMAND}, {'foreign': 'do not overwrite'}):
            with self.subTest(existing=existing):
                self.registry.live = copy.deepcopy(existing)
                self.path.unlink(missing_ok=True)
                with self.assertRaises(WorkaroundError):
                    self.install()
                self.assertEqual(self.registry.live, existing)
                self.assertEqual(self.registry.sets, 0)
                self.assertIsNone(json.loads(self.path.read_text())['prior_key_exists'])

    def test_old_journal_never_overwritten(self):
        self.path.write_text('old journal')
        with self.assertRaises(FileExistsError):
            self.install()
        self.assertEqual(self.path.read_text(), 'old journal')
        self.assertEqual(self.registry.sets, 0)

    def test_unsupported_transactions_fail_without_write(self):
        self.registry.unsupported = True
        with self.assertRaises(WorkaroundError):
            self.install()
        self.assertIsNone(self.registry.live)
        self.assertEqual(self.registry.sets, 0)

    def test_setup_denied_and_commit_failure_have_no_fallback(self):
        for failure in ('deny_set', 'fail_commit'):
            with self.subTest(failure=failure):
                self.registry = FakeRegistry()
                self.path.unlink(missing_ok=True)
                setattr(self.registry, failure, True)
                with self.assertRaises(WorkaroundError):
                    self.install()
                self.assertIsNone(self.registry.live)
                with self.assertRaises(WorkaroundError):
                    self.clean()
                self.assertEqual(self.registry.deletes, 0)

    def test_setup_conflict_preserves_concurrent_identical_value(self):
        # This models conflict BEFORE setup commit, unlike indistinguishable
        # same-value writes BETWEEN committed transactions (documented limit).
        self.registry.conflict = True
        with self.assertRaises(WorkaroundError):
            self.install()
        self.registry.live = {'command': COMMAND}  # Not our committed state.
        with self.assertRaises(WorkaroundError):
            self.clean()
        self.assertEqual(self.registry.live, {'command': COMMAND})
        self.assertEqual(self.registry.deletes, 0)

    def test_lost_postcommit_journal_is_not_adopted(self):
        original = module.atomic_state
        def write(path, record, exclusive=False):
            if record['phase'] == 'installed':
                raise OSError('journal failure')
            original(path, record, exclusive)
        with patch.object(module, 'atomic_state', side_effect=write), self.assertRaises(OSError):
            self.install()
        self.assertEqual(self.registry.live['command'], COMMAND)
        with self.assertRaises(WorkaroundError):
            self.clean()
        self.assertEqual(self.registry.deletes, 0)

    def test_wrong_run_head_attempt_command_or_target_blocks_cleanup(self):
        self.install()
        original = json.loads(self.path.read_text())
        variants = [dict(original, command='different'), dict(original, target='different'),
                    dict(original, prior_key_exists=True), dict(original, version=2)]
        for key in BINDING:
            variants.append(dict(original, binding=dict(BINDING, **{key: 'different'})))
        for record in variants:
            with self.subTest(record=record):
                self.path.write_text(json.dumps(record))
                with self.assertRaises(WorkaroundError):
                    self.clean()
        self.assertEqual(self.registry.deletes, 0)

    def test_any_observed_shape_metadata_or_value_drift_blocks_cleanup(self):
        self.install()
        original = copy.deepcopy(self.registry.live)
        for key, value in (('command', 'different'), ('values', 2), ('subkeys', 1),
                           ('class_size', 3), ('security_size', 90), ('last_write', 21),
                           ('canonical', 'other')):
            with self.subTest(key=key):
                self.registry.live = dict(original, **{key: value})
                with self.assertRaises(WorkaroundError):
                    self.clean()
                self.assertEqual(self.registry.live[key], value)
        self.assertEqual(self.registry.deletes, 0)

    def test_missing_key_and_unexpectedly_missing_debugger_are_failures(self):
        self.install()
        for state in (None, {}):
            self.registry.live = state
            with self.assertRaises(WorkaroundError):
                self.clean()
        self.assertEqual(self.registry.deletes, 0)

    def test_cleanup_delete_denied_does_not_claim_restoration(self):
        self.install()
        self.registry.deny_delete = True
        with self.assertRaises(WorkaroundError):
            self.clean()
        self.assertEqual(self.registry.live['command'], COMMAND)
        self.assertEqual(json.loads(self.path.read_text())['phase'], 'installed')

    def test_cleanup_conflict_or_commit_failure_never_clobbers_external_state(self):
        for failure in ('conflict', 'fail_commit'):
            with self.subTest(failure=failure):
                self.registry = FakeRegistry()
                self.path.unlink(missing_ok=True)
                self.install()
                setattr(self.registry, failure, True)
                with self.assertRaises(WorkaroundError):
                    self.clean()
                if failure == 'conflict':
                    self.assertEqual(self.registry.live, {'foreign': 'preserve concurrent writer'})
                else:
                    self.assertEqual(self.registry.live['command'], COMMAND)
                self.assertEqual(json.loads(self.path.read_text())['phase'], 'installed')

    def test_cleaned_journal_does_not_authorize_second_cleanup(self):
        self.install()
        self.clean()
        with self.assertRaises(WorkaroundError):
            self.clean()
        self.assertEqual(self.registry.deletes, 1)


class NativeBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.registry = NativeRegistry.__new__(NativeRegistry)
        self.registry.advapi = Mock()
        self.registry.ktm = Mock()
        self.registry.kernel = Mock()
        self.registry.ntdll = Mock()
        self.registry.transacted = set()
        self.registry.kernel.CloseHandle.return_value = 1
        self.registry.ktm.RollbackTransaction.return_value = 1
        self.registry.ktm.CommitTransaction.return_value = 1
        self.registry.advapi.RegCloseKey.return_value = 0
        self.error = patch.object(ctypes, 'get_last_error', return_value=5, create=True)
        self.error.start()
        self.addCleanup(self.error.stop)

    def test_write_and_delete_require_transacted_handle(self):
        with self.assertRaises(WorkaroundError):
            self.registry.set_debugger(7, COMMAND)
        with self.assertRaises(WorkaroundError):
            self.registry.delete_debugger(7)
        self.registry.advapi.RegSetValueExW.assert_not_called()
        self.registry.advapi.RegDeleteValueW.assert_not_called()

    def test_exact_utf16_reg_sz_and_only_named_value(self):
        self.registry.transacted.add(7)
        self.registry.advapi.RegSetValueExW.return_value = 0
        self.registry.advapi.RegDeleteValueW.return_value = 0
        self.registry.set_debugger(7, COMMAND)
        args = self.registry.advapi.RegSetValueExW.call_args.args
        self.assertEqual(args[:4], (7, 'Debugger', 0, 1))
        self.assertEqual(args[4].raw, expected_data(COMMAND))
        self.assertEqual(args[5], len(expected_data(COMMAND)))
        self.registry.delete_debugger(7)
        self.registry.advapi.RegDeleteValueW.assert_called_once_with(7, 'Debugger')

    def test_transaction_creation_failure_never_yields(self):
        for result in (None, ctypes.c_void_p(-1).value):
            self.registry.ktm.CreateTransaction.return_value = result
            with self.assertRaises(WorkaroundError), self.registry.transaction():
                self.fail('Unsupported transaction yielded')
        self.registry.ktm.CommitTransaction.assert_not_called()

    def test_commit_and_rollback_are_checked_without_retry(self):
        self.registry.ktm.CreateTransaction.return_value = 17
        with self.registry.transaction() as transaction:
            self.registry.commit(transaction)
        self.registry.ktm.CommitTransaction.assert_called_once_with(17)
        self.registry.ktm.RollbackTransaction.assert_not_called()
        self.registry.ktm.CommitTransaction.return_value = 0
        with self.assertRaises(WorkaroundError), self.registry.transaction() as transaction:
            self.registry.commit(transaction)
        self.registry.ktm.RollbackTransaction.assert_called_once_with(17)

    def test_rollback_or_close_failure_is_not_silent(self):
        self.registry.ktm.CreateTransaction.return_value = 17
        self.registry.ktm.RollbackTransaction.return_value = 0
        with self.assertRaises(WorkaroundError), self.registry.transaction():
            pass
        self.registry.ktm.RollbackTransaction.return_value = 1
        self.registry.kernel.CloseHandle.return_value = 0
        with self.assertRaises(WorkaroundError), self.registry.transaction() as transaction:
            self.registry.commit(transaction)

    def test_create_opens_only_direct_native_subkey_and_rejects_existing(self):
        def create(*args):
            self.assertEqual(args[0:5], (7, 'wsl.exe', 0, None, 0))
            self.assertEqual(args[5], module.KEY_READ | module.KEY_SET_VALUE | module.NATIVE_VIEW)
            self.assertIsNone(args[6])  # No custom ACL/security descriptor.
            self.assertEqual(args[-2:], (19, None))
            args[7]._obj.value = 22
            args[8]._obj.value = 2  # Existing empty/configured key both rejected.
            return 0
        self.registry.advapi.RegCreateKeyTransactedW.side_effect = create
        with self.assertRaises(WorkaroundError), self.registry.create_new(7, {'handle': 19}):
            self.fail('Existing key admitted')
        self.registry.advapi.RegSetValueExW.assert_not_called()
        self.registry.advapi.RegCloseKey.assert_called_once_with(22)
        self.assertFalse(self.registry.transacted)

    def test_native_metadata_explicit_empty_class_buffer(self):
        def query(*args):
            self.assertEqual(args[0], 7)
            self.assertEqual(len(args[1]), 1)
            self.assertEqual(args[2]._obj.value, 1)
            args[2]._obj.value = 0
            args[4]._obj.value = 0
            args[7]._obj.value = 1
            args[10]._obj.value = 88
            args[11]._obj.dwLowDateTime = 123
            return 0
        self.registry.advapi.RegQueryInfoKeyW.side_effect = query
        with patch.object(module, 'canonical_key_name', return_value=CANONICAL):
            result = self.registry.metadata(7)
        self.assertEqual(result['class_size'], 0)
        self.assertEqual(result['last_write'], 123)
        self.assertEqual(result['security_size'], 88)

    def test_native_nonempty_class_or_overflow_fails_without_retry(self):
        for status in (0, 234):  # Unexpected size, or ERROR_MORE_DATA.
            def query(*args):
                args[2]._obj.value = 2
                return status
            self.registry.advapi.RegQueryInfoKeyW.reset_mock()
            self.registry.advapi.RegQueryInfoKeyW.side_effect = query
            with patch.object(module, 'canonical_key_name', return_value=CANONICAL):
                with self.assertRaises(WorkaroundError):
                    self.registry.metadata(7)
            self.assertEqual(self.registry.advapi.RegQueryInfoKeyW.call_count, 1)

    def test_native_snapshot_never_logs_or_accepts_unexpected_value(self):
        metadata = {'canonical': CANONICAL.lower(), 'values': 1, 'subkeys': 0, 'class_size': 0}
        self.registry.metadata = Mock(return_value=metadata)
        def query(key, name, reserved, kind, buffer, size):
            kind._obj.value = 1
            size._obj.value = len(expected_data(COMMAND))
            if buffer is not None:
                ctypes.memmove(buffer, b'X' * size._obj.value, size._obj.value)
            return 0
        self.registry.advapi.RegQueryValueExW.side_effect = query
        with self.assertRaisesRegex(WorkaroundError, 'differs from exact approved value'):
            self.registry.snapshot(7, COMMAND)

    def test_native_snapshot_exact_value_and_midread_drift(self):
        metadata = {'canonical': CANONICAL.lower(), 'values': 1, 'subkeys': 0, 'class_size': 0}
        self.registry.metadata = Mock(return_value=metadata)
        def query(key, name, reserved, kind, buffer, size):
            kind._obj.value = 1
            size._obj.value = len(expected_data(COMMAND))
            if buffer is not None:
                ctypes.memmove(buffer, expected_data(COMMAND), size._obj.value)
            return 0
        self.registry.advapi.RegQueryValueExW.side_effect = query
        result = self.registry.snapshot(7, COMMAND)
        self.assertIn('debugger_sha256', result)
        self.registry.metadata.side_effect = [metadata, dict(metadata, values=2)]
        with self.assertRaisesRegex(WorkaroundError, 'change while reading'):
            self.registry.snapshot(7, COMMAND)

    def test_unexpected_type_or_size_not_read(self):
        self.registry.metadata = Mock(return_value={'values': 1, 'subkeys': 0, 'class_size': 0})
        for kind_value, size_value in ((2, 100), (1, 1000000)):
            def query(key, name, reserved, kind, buffer, size):
                self.assertIsNone(buffer)
                kind._obj.value = kind_value
                size._obj.value = size_value
                return 0
            self.registry.advapi.RegQueryValueExW.side_effect = query
            with self.assertRaises(WorkaroundError):
                self.registry.snapshot(7, COMMAND)


class WorkflowTests(unittest.TestCase):
    def test_manual_default_off_first_attempt_and_exact_approval_head(self):
        source = (SCRIPTS.parent / 'workflows/packaged-gui.yml').read_text()
        option = source.split('      temporary_wsl_ifeo:', 1)[1].split('      wsl_ifeo_approved_head:', 1)[0]
        self.assertIn('default: false', option)
        self.assertIn('ALL WSL launches', option)
        self.assertIn("github.event_name == 'workflow_dispatch' && inputs.temporary_wsl_ifeo && matrix.runner == 'windows-11-arm'", source)
        self.assertNotIn('temporary_wsl_ifeo', source.split('  workflow_call:', 1)[1].split('  workflow_dispatch:', 1)[0])
        self.assertNotIn('temporary_wsl_ifeo', (SCRIPTS.parent / 'workflows/cd.yml').read_text())

    def test_early_setup_always_cleanup_and_all_nine_matrix_entries_retained(self):
        source = (SCRIPTS.parent / 'workflows/packaged-gui.yml').read_text()
        self.assertLess(source.index('windows_wsl_ifeo.py setup'), source.index('actions/setup-python'))
        self.assertLess(source.index('windows_wsl_ifeo.py cleanup'), source.index('- name: Save GUI diagnostics'))
        self.assertIn("if: always() && env.TEXSTUDIO_TEMPORARY_WSL_IFEO == '1'", source)
        self.assertEqual(source.count('          - runner:'), 4)
        self.assertEqual(source.count('          - name:'), 5)
        self.assertIn("format('-temporary-wsl-{0}', github.run_id)", source)
        self.assertIn('python .github/scripts/smoke-native-package.py', source)

    def test_no_key_delete_or_acl_or_nontransactional_fallback_api(self):
        source = (SCRIPTS / 'windows_wsl_ifeo.py').read_text()
        for forbidden in ('RegDeleteKey', 'RegSetKeySecurity', 'SetSecurityInfo',
                          'RegCreateKeyEx', 'winreg.SetValue', 'winreg.DeleteValue', 'LITERAL_WOW'):
            self.assertNotIn(forbidden, source)
        self.assertIn('RegCreateKeyTransactedW', source)
        self.assertIn('RegOpenKeyTransactedW', source)


if __name__ == '__main__':
    unittest.main()
