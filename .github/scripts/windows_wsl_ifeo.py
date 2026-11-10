#!/usr/bin/env python3
"""One explicitly approved disposable ARM run only; no nontransactional fallback.

Exact-state cleanup is not proof of historical ownership: identical external
rewrites between transactions cannot be distinguished. Never use on a user PC.
"""
import argparse
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import sys

from inspect_windows_wsl_registry import BASE, canonical_key_name, system_metadata

PARENT = BASE.rsplit('\\', 1)[0]
CANONICAL = '\\REGISTRY\\MACHINE\\' + BASE
BUILD_RUN = '42832542074'
BUILD_SHA = '066c2c8c09e9995129efdc903d3480a8e9c72255'
KEY_READ, KEY_SET_VALUE, KEY_CREATE_SUB_KEY, NATIVE_VIEW = 0x20019, 2, 4, 0x100
REG_SZ = 1


class WorkaroundError(RuntimeError):
    pass


def context(environment, system_platform):
    required = {'GITHUB_ACTIONS': 'true', 'RUNNER_ENVIRONMENT': 'github-hosted',
                'RUNNER_OS': 'Windows', 'RUNNER_ARCH': 'ARM64',
                'GITHUB_EVENT_NAME': 'workflow_dispatch',
                'GITHUB_REPOSITORY': 'feiyang-cai/texstudio-vim',
                'GITHUB_WORKFLOW': 'Packaged desktop retest', 'GITHUB_JOB': 'desktop',
                'GITHUB_RUN_ATTEMPT': '1', 'TEXSTUDIO_TEMPORARY_WSL_IFEO': '1',
                'TEXSTUDIO_BUILD_RUN_ID': BUILD_RUN, 'BUILD_SHA': BUILD_SHA}
    if system_platform != 'win32' or any(environment.get(k) != v for k, v in required.items()):
        raise WorkaroundError('Requires the approved first-attempt manual hosted ARM original-package run')
    sha, run = environment.get('GITHUB_SHA', ''), environment.get('GITHUB_RUN_ID', '')
    if not re.fullmatch('[0-9a-f]{40}', sha) or not re.fullmatch('[1-9][0-9]*', run):
        raise WorkaroundError('Missing exact workflow head/run identity')
    if environment.get('TEXSTUDIO_IFEO_APPROVED_HEAD') != sha:
        raise WorkaroundError('Approval head does not match this workflow head')
    return {'repository': required['GITHUB_REPOSITORY'], 'run_id': run,
            'attempt': '1', 'head': sha, 'build_run': BUILD_RUN, 'build_sha': BUILD_SHA}


def approved_command(system):
    systray = system['systray']
    if system['python_bits'] != 64 or not systray['is_file']:
        raise WorkaroundError('Native built-in systray.exe is unavailable; no install or fallback')
    # system_metadata obtains System32 from GetSystemDirectoryW, not user input.
    path = systray['path']
    if not re.fullmatch(r'[A-Za-z]:\\[^"\r\n]*\\system32\\systray\.exe', path, re.IGNORECASE):
        raise WorkaroundError('Unexpected native systray path')
    return '"' + path + '"'


def expected_data(command):
    return (command + '\0').encode('utf-16-le')


def atomic_state(path, record, exclusive=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    if exclusive:
        with path.open('x', encoding='utf-8') as stream:
            json.dump(record, stream, indent=2)
            stream.flush()
            os.fsync(stream.fileno())
    else:
        temp = path.with_suffix('.tmp')
        with temp.open('w', encoding='utf-8') as stream:
            json.dump(record, stream, indent=2)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)


def setup(registry, binding, command, state_path):
    # Never adopt or overwrite an earlier journal, including an interrupted run.
    record = {'version': 1, 'binding': binding, 'command': command, 'target': BASE,
              'prior_key_exists': None, 'phase': 'prepared'}
    atomic_state(state_path, record, exclusive=True)
    with registry.transaction() as transaction:
        with registry.parent(transaction) as parent:
            with registry.create_new(parent, transaction) as key:
                record['prior_key_exists'] = False  # Established by CREATED_NEW_KEY.
                registry.set_debugger(key, command)
                registry.snapshot(key, command)  # Validate the transacted shape/value.
        record['phase'] = 'commit-pending'
        atomic_state(state_path, record)
        registry.commit(transaction)
    # Capture the exact committed state, not an assumed commit timestamp.
    # A same-value rewrite before this read is indistinguishable (documented).
    with registry.open_target() as key:
        record['expected'] = registry.snapshot(key, command)
    record['phase'] = 'installed'
    atomic_state(state_path, record)
    return record


def cleanup(registry, binding, command, state_path):
    record = json.loads(state_path.read_text(encoding='utf-8'))
    if (record.get('version') != 1 or record.get('binding') != binding or
            record.get('command') != command or record.get('target') != BASE or
            record.get('prior_key_exists') is not False):
        raise WorkaroundError('Cleanup journal does not match this exact approved run/target')
    if record.get('phase') != 'installed' or not isinstance(record.get('expected'), dict):
        # In particular, never infer ownership from equality after an uncertain
        # setup commit or a lost journal write. The disposable VM must be retired.
        raise WorkaroundError('Cleanup cannot establish committed setup state; no registry change attempted')
    with registry.transaction() as transaction:
        with registry.open_target(transaction) as key:
            if registry.snapshot(key, command) != record['expected']:
                raise WorkaroundError('Observed IFEO state drift; cleanup refuses to change it')
            registry.delete_debugger(key)
            registry.require_empty(key)
        registry.commit(transaction)
    # Keep the key shell. Never perform inspect-empty then non-atomic key deletion.
    with registry.open_target() as key:
        registry.require_empty(key)
    record['phase'] = 'cleaned'
    record['empty_key_left'] = True
    atomic_state(state_path, record)
    return record


class NativeRegistry:
    """All value writes use handles enlisted in a native KTM transaction."""
    def __init__(self):
        self.advapi = ctypes.WinDLL('advapi32', use_last_error=True)
        self.ktm = ctypes.WinDLL('ktmw32', use_last_error=True)
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.ntdll = ctypes.WinDLL('ntdll')
        H, D, S, P = wintypes.HANDLE, wintypes.DWORD, wintypes.LPCWSTR, ctypes.c_void_p
        PH, PD = ctypes.POINTER(H), ctypes.POINTER(D)
        signatures = {
            'RegOpenKeyTransactedW': ([H, S, D, D, PH, H, P], wintypes.LONG),
            'RegCreateKeyTransactedW': ([H, S, D, wintypes.LPWSTR, D, D, P, PH, PD, H, P], wintypes.LONG),
            'RegOpenKeyExW': ([H, S, D, D, PH], wintypes.LONG),
            'RegCloseKey': ([H], wintypes.LONG),
            'RegSetValueExW': ([H, S, D, D, P, D], wintypes.LONG),
            'RegQueryValueExW': ([H, S, P, PD, P, PD], wintypes.LONG),
            'RegDeleteValueW': ([H, S], wintypes.LONG),
            'RegQueryInfoKeyW': ([H, wintypes.LPWSTR, PD, P, PD, PD, PD, PD, PD, PD, PD,
                                 ctypes.POINTER(wintypes.FILETIME)], wintypes.LONG),
        }
        for name, (args, result) in signatures.items():
            function = getattr(self.advapi, name)
            function.argtypes, function.restype = args, result
        self.ktm.CreateTransaction.argtypes = [P, P, D, D, D, D, wintypes.LPWSTR]
        self.ktm.CreateTransaction.restype = H
        for name in ('CommitTransaction', 'RollbackTransaction'):
            getattr(self.ktm, name).argtypes = [H]
            getattr(self.ktm, name).restype = wintypes.BOOL
        self.kernel.CloseHandle.argtypes = [H]
        self.kernel.CloseHandle.restype = wintypes.BOOL
        self.transacted = set()

    @staticmethod
    def check(status, action):
        if status:
            raise WorkaroundError(f'{action} failed with native status {status}; no fallback')

    @contextmanager
    def transaction(self):
        handle = self.ktm.CreateTransaction(None, None, 0, 0, 0, 30000, 'Temporary CI WSL redirect')
        if not handle or handle == ctypes.c_void_p(-1).value:
            raise WorkaroundError(f'CreateTransaction failed with error {ctypes.get_last_error()}; no fallback')
        transaction = {'handle': handle, 'committed': False}
        try:
            yield transaction
        finally:
            try:
                if not transaction['committed'] and not self.ktm.RollbackTransaction(handle):
                    raise WorkaroundError(f'RollbackTransaction failed with error {ctypes.get_last_error()}')
            finally:
                if not self.kernel.CloseHandle(handle):
                    raise WorkaroundError(f'Transaction CloseHandle failed with error {ctypes.get_last_error()}')

    def commit(self, transaction):
        if not self.ktm.CommitTransaction(transaction['handle']):
            raise WorkaroundError(f'CommitTransaction failed with error {ctypes.get_last_error()}; no retry')
        transaction['committed'] = True

    @contextmanager
    def held_key(self, key, transaction=None):
        if transaction is not None:
            self.transacted.add(key)
        try:
            yield key
        finally:
            self.transacted.discard(key)
            self.check(self.advapi.RegCloseKey(key), 'RegCloseKey')

    def open(self, path, access, transaction=None):
        handle = wintypes.HANDLE()
        # Sign-extend predefined HKLM exactly as the Windows header defines it.
        root = ctypes.c_void_p(ctypes.c_int32(0x80000002).value)
        if transaction is None:
            status = self.advapi.RegOpenKeyExW(root, path, 0, access | NATIVE_VIEW, ctypes.byref(handle))
        else:
            status = self.advapi.RegOpenKeyTransactedW(root, path, 0, access | NATIVE_VIEW,
                                                      ctypes.byref(handle), transaction['handle'], None)
        self.check(status, 'Open exact IFEO key')
        return self.held_key(handle.value, transaction)

    @contextmanager
    def parent(self, transaction):
        with self.open(PARENT, KEY_READ | KEY_CREATE_SUB_KEY, transaction) as key:
            if canonical_key_name(self.ntdll, key).lower() != CANONICAL.rsplit('\\', 1)[0].lower():
                raise WorkaroundError('Native IFEO parent does not match inspected physical key')
            yield key

    @contextmanager
    def create_new(self, parent, transaction):
        key, disposition = wintypes.HANDLE(), wintypes.DWORD()
        self.check(self.advapi.RegCreateKeyTransactedW(
            parent, 'wsl.exe', 0, None, 0, KEY_READ | KEY_SET_VALUE | NATIVE_VIEW,
            None, ctypes.byref(key), ctypes.byref(disposition), transaction['handle'], None),
            'Create transacted wsl.exe key')
        with self.held_key(key.value, transaction) as handle:
            if disposition.value != 1:  # REG_CREATED_NEW_KEY, not OPENED_EXISTING_KEY.
                raise WorkaroundError('Preexisting wsl.exe key is not accepted, even if empty')
            self.require_empty(handle)
            yield handle

    def open_target(self, transaction=None):
        access = KEY_READ | (KEY_SET_VALUE if transaction is not None else 0)
        return self.open(BASE, access, transaction)

    def metadata(self, key):
        name = canonical_key_name(self.ntdll, key)
        if name.lower() != CANONICAL.lower():
            raise WorkaroundError('Unexpected physical IFEO target')
        subkeys, values, class_size, security_size = (wintypes.DWORD() for _ in range(4))
        stamp = wintypes.FILETIME()
        empty_class = ctypes.create_unicode_buffer(1)
        class_size.value = 1  # Only an empty class plus its NUL fits.
        self.check(self.advapi.RegQueryInfoKeyW(
            key, empty_class, ctypes.byref(class_size), None, ctypes.byref(subkeys), None, None,
            ctypes.byref(values), None, None, ctypes.byref(security_size), ctypes.byref(stamp)),
            'Query target metadata')
        if class_size.value != 0:
            raise WorkaroundError('Unexpected nonempty IFEO key class')
        return {'canonical': name.lower(), 'subkeys': subkeys.value, 'values': values.value,
                'class_size': class_size.value, 'security_size': security_size.value,
                'last_write': (stamp.dwHighDateTime << 32) | stamp.dwLowDateTime}

    def require_empty(self, key):
        meta = self.metadata(key)
        if meta['subkeys'] or meta['values'] or meta['class_size']:
            raise WorkaroundError('Unexpected nonempty IFEO key state')

    def snapshot(self, key, command):
        before = self.metadata(key)
        if before['subkeys'] or before['values'] != 1 or before['class_size']:
            raise WorkaroundError('Unexpected IFEO value/subkey shape')
        desired = expected_data(command)
        kind, size = wintypes.DWORD(), wintypes.DWORD()
        self.check(self.advapi.RegQueryValueExW(key, 'Debugger', None, ctypes.byref(kind), None,
                                             ctypes.byref(size)), 'Query Debugger metadata')
        if kind.value != REG_SZ or size.value != len(desired):
            raise WorkaroundError('Debugger type/size differs from exact approved value')
        buffer = ctypes.create_string_buffer(len(desired))
        self.check(self.advapi.RegQueryValueExW(key, 'Debugger', None, ctypes.byref(kind), buffer,
                                             ctypes.byref(size)), 'Read expected Debugger')
        if kind.value != REG_SZ or size.value != len(desired) or buffer.raw != desired:
            raise WorkaroundError('Debugger differs from exact approved value')
        if self.metadata(key) != before:
            raise WorkaroundError('Observed IFEO change while reading state')
        return dict(before, debugger_sha256=hashlib.sha256(desired).hexdigest())

    def set_debugger(self, key, command):
        if key not in self.transacted:
            raise WorkaroundError('Refusing nontransactional registry write')
        data = expected_data(command)
        buffer = ctypes.create_string_buffer(data, len(data))
        self.check(self.advapi.RegSetValueExW(key, 'Debugger', 0, REG_SZ, buffer, len(data)),
                   'Set exact transacted Debugger')

    def delete_debugger(self, key):
        if key not in self.transacted:
            raise WorkaroundError('Refusing nontransactional registry delete')
        self.check(self.advapi.RegDeleteValueW(key, 'Debugger'), 'Delete exact transacted Debugger')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('setup', 'cleanup'))
    args = parser.parse_args()
    report_path = Path('native-smoke-results') / ('wsl-ifeo-' + args.action + '.json')
    report = {'action': args.action, 'target': BASE,
              'status': 'failed', 'native_validation_required': True}
    try:
        binding = context(os.environ, sys.platform)
        report['binding'] = binding
        state = Path(os.environ['RUNNER_TEMP']) / 'texstudio-wsl-ifeo-state.json'
        command = approved_command(system_metadata())
        record = {'setup': setup, 'cleanup': cleanup}[args.action](NativeRegistry(), binding, command, state)
        report.update(status='passed', phase=record['phase'], command=command,
                      empty_key_left=record.get('empty_key_left', False))
    except Exception as error:
        # Do not print arbitrary registry values, arguments, or exception strings.
        report['error'] = {'kind': type(error).__name__, 'winerror': getattr(error, 'winerror', None)}
        if isinstance(error, WorkaroundError):
            report['error']['reason'] = str(error)
        raise SystemExit('Temporary IFEO ' + args.action + ' failed; see diagnostic report') from None
    finally:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2), encoding='utf-8')
        print(json.dumps(report))


if __name__ == '__main__':
    main()
