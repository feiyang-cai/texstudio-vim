"""Read-only inspector tests with fake APIs; no Windows APIs are invoked."""
import ast
import ctypes
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
from inspect_windows_wsl_registry import (BASE, LITERAL_WOW, TARGETS, InspectionError,
                                         canonical_key_name, debugger_metadata,
                                         inspect_targets, validate_context)

ENVIRONMENT = {'GITHUB_ACTIONS': 'true', 'RUNNER_ENVIRONMENT': 'github-hosted',
               'RUNNER_OS': 'Windows', 'RUNNER_ARCH': 'ARM64',
               'GITHUB_EVENT_NAME': 'workflow_dispatch',
               'GITHUB_REPOSITORY': 'feiyang-cai/texstudio-vim',
               'GITHUB_WORKFLOW': 'Packaged desktop retest',
               'TEXSTUDIO_WSL_REGISTRY_INSPECTION': '1'}


class ContextTests(unittest.TestCase):
    def test_exact_diagnostic_context(self):
        validate_context(ENVIRONMENT, 'win32')

    def test_every_required_field_and_platform(self):
        for key in ENVIRONMENT:
            with self.subTest(field=key), self.assertRaises(InspectionError):
                validate_context({k: v for k, v in ENVIRONMENT.items() if k != key}, 'win32')
        with self.assertRaises(InspectionError):
            validate_context(ENVIRONMENT, 'linux')

    def test_nonmanual_and_user_machine_rejected(self):
        for update in ({'GITHUB_EVENT_NAME': 'push'}, {'GITHUB_EVENT_NAME': 'pull_request'},
                       {'GITHUB_EVENT_NAME': 'workflow_call'}, {'RUNNER_ENVIRONMENT': 'self-hosted'},
                       {'RUNNER_ARCH': 'X64'}, {'TEXSTUDIO_WSL_REGISTRY_INSPECTION': '0'}):
            with self.subTest(update=update), self.assertRaises(InspectionError):
                validate_context(dict(ENVIRONMENT, **update), 'win32')


class NativeMetadataTests(unittest.TestCase):
    def test_debugger_presence_and_type_without_value_buffer(self):
        api = Mock()
        def query(handle, name, reserved, kind, data, size):
            self.assertEqual(handle, 7)
            self.assertEqual(name, 'Debugger')
            self.assertIsNone(data)
            kind._obj.value = 1
            size._obj.value = 1000  # Content must not be fetched, regardless of size.
            return 0
        api.RegQueryValueExW.side_effect = query
        self.assertEqual(debugger_metadata(api, 7), {'present': True, 'type': 1})
        self.assertEqual(api.RegQueryValueExW.call_count, 1)

    def test_missing_debugger(self):
        api = Mock()
        api.RegQueryValueExW.return_value = 2
        self.assertEqual(debugger_metadata(api, 7), {'present': False, 'type': None})

    def test_denied_metadata_fails(self):
        api = Mock()
        api.RegQueryValueExW.return_value = 5
        with self.assertRaises(InspectionError):
            debugger_metadata(api, 7)

    def test_canonical_native_key_identity(self):
        api = Mock()
        name = r'\REGISTRY\MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options'
        encoded = name.encode('utf-16-le')
        response = len(encoded).to_bytes(4, 'little') + encoded
        def query(handle, information_class, buffer, size, required):
            self.assertEqual(information_class, 3)
            required._obj.value = len(response)
            if buffer is None:
                return 0xC0000023
            ctypes.memmove(buffer, response, len(response))
            return 0
        api.NtQueryKey.side_effect = query
        self.assertEqual(canonical_key_name(api, 7), name)

    def test_canonical_query_denial_and_unbounded_size_fail(self):
        for status, size in ((0xC0000022, 100), (0xC0000023, 1000000)):
            api = Mock()
            def query(*args):
                args[-1]._obj.value = size
                return status
            api.NtQueryKey.side_effect = query
            with self.subTest(status=status, size=size), self.assertRaises(InspectionError):
                canonical_key_name(api, 7)


class CollectionTests(unittest.TestCase):
    def test_exact_paths_only_and_aliases_are_observed_not_assumed(self):
        reader = Mock()
        reader.inspect.return_value = {'exists': True, 'canonical_name': 'same-shared-key'}
        result = inspect_targets(reader, {'machine': 'ARM64'})
        self.assertTrue(result['diagnostic_only'])
        self.assertFalse(result['release_validation'])
        self.assertEqual(result['status'], 'complete')
        self.assertEqual(len(result['targets']), 4)
        self.assertEqual({p for _, p, _ in TARGETS}, {BASE, LITERAL_WOW})
        self.assertEqual(reader.inspect.call_count, 8)
        allowed = {BASE, LITERAL_WOW, BASE.rsplit('\\', 1)[0], LITERAL_WOW.rsplit('\\', 1)[0]}
        self.assertTrue(all(call.args[0] in allowed for call in reader.inspect.call_args_list))
        self.assertTrue(all(row['key']['canonical_name'] == 'same-shared-key' for row in result['targets']))

    def test_missing_keys_are_valid_evidence(self):
        reader = Mock()
        reader.inspect.return_value = {'exists': False}
        result = inspect_targets(reader, {})
        self.assertEqual(result['status'], 'complete')
        self.assertTrue(all(not row['key']['exists'] for row in result['targets']))

    def test_access_failure_is_incomplete_without_arbitrary_exception_data(self):
        reader = Mock()
        reader.inspect.side_effect = PermissionError('do not publish arbitrary data')
        result = inspect_targets(reader, {})
        self.assertEqual(result['status'], 'incomplete')
        self.assertEqual(len(result['targets']), 4)
        self.assertNotIn('do not publish', str(result))


class RoutingAndBoundaryTests(unittest.TestCase):
    def test_default_off_and_diagnostic_run_skips_selector_only_explicitly(self):
        source = (SCRIPTS.parent / 'workflows/packaged-gui.yml').read_text()
        option = source.split('      inspect_wsl_registry_only:', 1)[1].split('permissions:', 1)[0]
        self.assertIn('type: boolean', option)
        self.assertIn('default: false', option)
        self.assertIn("if: github.event_name == 'workflow_dispatch' && inputs.inspect_wsl_registry_only", source)
        self.assertIn("  select:\n    if: github.event_name != 'workflow_dispatch' || !inputs.inspect_wsl_registry_only", source)
        self.assertEqual(source.count('    needs: select'), 2)
        self.assertEqual(source.count("    if: needs.select.outputs.run_id != ''"), 2)

    def test_diagnostic_job_is_not_package_validation_and_has_no_install(self):
        source = (SCRIPTS.parent / 'workflows/packaged-gui.yml').read_text()
        diagnostic = source.split('  inspect-wsl-registry:', 1)[1].split('  select:', 1)[0]
        self.assertIn('NOT release validation', diagnostic)
        self.assertIn('runs-on: windows-11-arm', diagnostic)
        for forbidden in ('setup-python', 'pip install', 'prepare-windows-gui', 'smoke-native-package',
                          'windows_wsl_ifeo', 'download-artifact'):
            self.assertNotIn(forbidden, diagnostic)
        self.assertNotIn('inspect_wsl_registry_only', (SCRIPTS.parent / 'workflows/cd.yml').read_text())

    def test_inspection_cannot_cancel_package_validation(self):
        source = (SCRIPTS.parent / 'workflows/packaged-gui.yml').read_text()
        self.assertIn("${{ github.event_name == 'workflow_dispatch' && inputs.inspect_wsl_registry_only && '-registry-inspection' || '' }}", source)

    def test_inspection_does_not_persist_checkout_credentials(self):
        source = (SCRIPTS.parent / 'workflows/packaged-gui.yml').read_text()
        diagnostic = source.split('  inspect-wsl-registry:', 1)[1].split('  select:', 1)[0]
        self.assertIn('persist-credentials: false', diagnostic)

    def test_no_mutating_registry_or_process_api_in_inspector(self):
        source = (SCRIPTS / 'inspect_windows_wsl_registry.py').read_text()
        tree = ast.parse(source)
        attributes = {node.attr for node in ast.walk(tree) if isinstance(node, ast.Attribute)}
        forbidden = {'CreateKey', 'CreateKeyEx', 'SetValue', 'SetValueEx', 'DeleteValue', 'DeleteKey',
                     'DeleteKeyEx', 'SetSecurityInfo', 'RegSetValueExW', 'RegDeleteValueW',
                     'RegCreateKeyExW', 'RegDeleteKeyExW', 'TerminateProcess', 'Popen', 'run', 'system'}
        self.assertFalse(attributes & forbidden)
        self.assertIn('api.KEY_READ | view', source)
        self.assertNotIn('api.QueryValueEx(', source)
        self.assertNotIn('KEY_WRITE', source)


if __name__ == '__main__':
    unittest.main()
