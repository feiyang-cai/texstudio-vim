#!/usr/bin/env python3
"""Read-only IFEO identity inspection. This is NOT package/release validation."""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import platform
import sys

BASE = r'SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\wsl.exe'
LITERAL_WOW = r'SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\wsl.exe'
# Logical alternate views may alias. The literal WOW path is a distinct question,
# not an assumed equivalent. Only these exact paths and their parents are opened.
TARGETS = (('logical_native', BASE, 0x100), ('logical_wow', BASE, 0x200),
           ('literal_native', BASE, 0x100), ('literal_wow', LITERAL_WOW, 0x100))


class InspectionError(RuntimeError):
    pass


def validate_context(environment, system_platform):
    expected = {'GITHUB_ACTIONS': 'true', 'RUNNER_ENVIRONMENT': 'github-hosted',
                'RUNNER_OS': 'Windows', 'RUNNER_ARCH': 'ARM64',
                'GITHUB_EVENT_NAME': 'workflow_dispatch',
                'GITHUB_REPOSITORY': 'feiyang-cai/texstudio-vim',
                'GITHUB_WORKFLOW': 'Packaged desktop retest',
                'TEXSTUDIO_WSL_REGISTRY_INSPECTION': '1'}
    if system_platform != 'win32' or any(environment.get(k) != v for k, v in expected.items()):
        raise InspectionError('Requires the explicitly selected hosted Windows ARM manual diagnostic job')


def debugger_metadata(api, handle):
    """Query presence/type only: lpData is NULL, so no value content is read."""
    api.RegQueryValueExW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, ctypes.c_void_p,
                                   ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p,
                                   ctypes.POINTER(wintypes.DWORD)]
    api.RegQueryValueExW.restype = wintypes.LONG
    kind, size = wintypes.DWORD(), wintypes.DWORD()
    status = api.RegQueryValueExW(int(handle), 'Debugger', None, ctypes.byref(kind), None, ctypes.byref(size))
    if status == 2:  # ERROR_FILE_NOT_FOUND
        return {'present': False, 'type': None}
    if status:
        raise InspectionError(f'Debugger metadata query failed with status {status}')
    return {'present': True, 'type': kind.value}


def canonical_key_name(api, handle):
    """Resolve an opened key's native object name without changing that key."""
    api.NtQueryKey.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                              wintypes.ULONG, ctypes.POINTER(wintypes.ULONG)]
    api.NtQueryKey.restype = wintypes.LONG
    size = wintypes.ULONG()
    status = api.NtQueryKey(int(handle), 3, None, 0, ctypes.byref(size)) & 0xffffffff
    if status not in (0xC0000023, 0x80000005) or not 4 <= size.value <= 65536:
        raise InspectionError(f'Canonical key-name sizing failed: 0x{status:08x}')
    buffer = ctypes.create_string_buffer(size.value)
    status = api.NtQueryKey(int(handle), 3, buffer, size.value, ctypes.byref(size)) & 0xffffffff
    if status:
        raise InspectionError(f'Canonical key-name query failed: 0x{status:08x}')
    length = int.from_bytes(buffer.raw[:4], 'little')
    if length % 2 or length > len(buffer) - 4:
        raise InspectionError('Invalid canonical key-name response')
    return buffer.raw[4:4 + length].decode('utf-16-le')


class ReadOnlyRegistry:
    def __init__(self):
        import winreg
        self.registry = winreg
        self.advapi = ctypes.WinDLL('advapi32', use_last_error=True)
        self.ntdll = ctypes.WinDLL('ntdll')

    def inspect(self, path, view, include_debugger=False):
        api = self.registry
        try:
            handle = api.OpenKey(api.HKEY_LOCAL_MACHINE, path, 0, api.KEY_READ | view)
        except FileNotFoundError:
            return {'exists': False}
        with handle:
            result = {'exists': True, 'canonical_name': canonical_key_name(self.ntdll, handle)}
            if include_debugger:
                result['debugger'] = debugger_metadata(self.advapi, handle)
            return result


def system_metadata():
    if ctypes.sizeof(ctypes.c_void_p) != 8:
        raise InspectionError('A preinstalled 64-bit Python is required; no installation is attempted')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetSystemDirectoryW.argtypes = [wintypes.LPWSTR, wintypes.UINT]
    buffer = ctypes.create_unicode_buffer(32768)
    length = kernel.GetSystemDirectoryW(buffer, len(buffer))
    if not 0 < length < len(buffer) or Path(buffer.value).name.lower() != 'system32':
        raise InspectionError('Native System32 directory could not be identified')
    target = Path(buffer.value) / 'systray.exe'
    return {'os': platform.platform(), 'machine': platform.machine(),
            'python_bits': ctypes.sizeof(ctypes.c_void_p) * 8,
            'systray': {'path': str(target), 'is_file': target.is_file()}}


def inspect_targets(reader, system):
    report = {'diagnostic_only': True, 'release_validation': False,
              'status': 'complete', 'system': system, 'targets': []}
    for label, path, view in TARGETS:
        item = {'label': label, 'path': path, 'view_flag': view}
        try:
            item['parent'] = reader.inspect(path.rsplit('\\', 1)[0], view)
            item['key'] = reader.inspect(path, view, include_debugger=True)
        except Exception as error:
            report['status'] = 'incomplete'
            # Only this helper's fixed messages or numeric native error codes.
            # Never dump arbitrary registry values or process command lines.
            item['error'] = {'kind': type(error).__name__, 'winerror': getattr(error, 'winerror', None)}
            if isinstance(error, InspectionError):
                item['error']['reason'] = str(error)
        report['targets'].append(item)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    validate_context(os.environ, sys.platform)
    report = inspect_targets(ReadOnlyRegistry(), system_metadata())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({'diagnostic_only': True, 'release_validation': False, 'status': report['status']}))
    if report['status'] != 'complete':
        raise SystemExit('Read-only registry inspection was incomplete; see its diagnostic artifact')


if __name__ == '__main__':
    main()
