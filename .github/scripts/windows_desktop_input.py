"""Windows desktop input using SendInput, including native ARM64 runners."""
import ctypes
from ctypes import wintypes
import time

class KeyboardInput(ctypes.Structure):
    _fields_ = [('vk', wintypes.WORD), ('scan', wintypes.WORD),
                ('flags', wintypes.DWORD), ('time', wintypes.DWORD),
                ('extra', ctypes.c_size_t)]

class MouseInput(ctypes.Structure):
    _fields_ = [('dx', wintypes.LONG), ('dy', wintypes.LONG),
                ('data', wintypes.DWORD), ('flags', wintypes.DWORD),
                ('time', wintypes.DWORD), ('extra', ctypes.c_size_t)]

class HardwareInput(ctypes.Structure):
    _fields_ = [('message', wintypes.DWORD), ('low', wintypes.WORD), ('high', wintypes.WORD)]

class InputUnion(ctypes.Union):
    _fields_ = [('keyboard', KeyboardInput), ('mouse', MouseInput), ('hardware', HardwareInput)]

class Input(ctypes.Structure):
    _fields_ = [('type', wintypes.DWORD), ('value', InputUnion)]

class WindowsDesktopInput:
    keys = {'esc': 0x1b, 'enter': 0x0d, 'ctrl': 0x11, 'shift': 0x10, 'alt': 0x12, 'tab': 0x09}

    def __init__(self):
        self.api = ctypes.windll.user32
        self.api.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(Input), ctypes.c_int]
        self.api.SendInput.restype = wintypes.UINT
        self.api.VkKeyScanW.argtypes = [wintypes.WCHAR]
        self.api.VkKeyScanW.restype = ctypes.c_short
        expected_size = 40 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28
        if ctypes.sizeof(Input) != expected_size:
            raise RuntimeError('Unexpected Win32 input structure layout')

    def send(self, item):
        if self.api.SendInput(1, ctypes.byref(item), ctypes.sizeof(Input)) != 1:
            raise RuntimeError('Windows rejected SendInput; check the interactive desktop and integrity level')

    def key(self, vk, up=False):
        self.send(Input(type=1, value=InputUnion(keyboard=KeyboardInput(vk=vk, flags=2 if up else 0))))

    def scan(self, char):
        result = self.api.VkKeyScanW(char)
        if result == -1:
            raise RuntimeError(f'Keyboard layout cannot type {char!r}')
        return result & 0xff, (result >> 8) & 0xff

    def press(self, name):
        vk = self.keys[name] if name in self.keys else self.scan(name)[0]
        self.key(vk)
        self.key(vk, up=True)

    def write(self, text, interval=0):
        for char in text:
            vk, mask = self.scan(char)
            modifiers = [code for bit, code in [(1, 0x10), (2, 0x11), (4, 0x12)] if mask & bit]
            try:
                for code in modifiers:
                    self.key(code)
                self.key(vk)
                self.key(vk, up=True)
            finally:
                for code in reversed(modifiers):
                    self.key(code, up=True)
            time.sleep(interval)

    def hotkey(self, *names):
        codes = [self.keys[name] if name in self.keys else self.scan(name)[0] for name in names]
        try:
            for code in codes:
                self.key(code)
        finally:
            for code in reversed(codes):
                self.key(code, up=True)

    def click(self, x, y):
        if not self.api.SetCursorPos(int(x), int(y)):
            raise RuntimeError('Windows rejected the desktop mouse position')
        self.send(Input(type=0, value=InputUnion(mouse=MouseInput(flags=2))))
        self.send(Input(type=0, value=InputUnion(mouse=MouseInput(flags=4))))

    def activate(self, hwnd):
        api = self.api
        api.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD)]
        api.GetWindowThreadProcessId.restype = wintypes.DWORD
        api.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
        api.SetFocus.argtypes = [ctypes.c_void_p]
        api.SetFocus.restype = ctypes.c_void_p
        api.BringWindowToTop.argtypes = [ctypes.c_void_p]
        message = wintypes.MSG()
        api.PeekMessageW(ctypes.byref(message), None, 0, 0, 0)
        current = ctypes.windll.kernel32.GetCurrentThreadId()
        target = api.GetWindowThreadProcessId(hwnd, None)
        foreground = api.GetWindowThreadProcessId(api.GetForegroundWindow(), None)
        attached = []
        try:
            for thread in {target, foreground} - {current, 0}:
                if api.AttachThreadInput(current, thread, True):
                    attached.append(thread)
            api.ShowWindow(hwnd, 9)
            api.BringWindowToTop(hwnd)
            api.SetForegroundWindow(hwnd)
            api.SetFocus(hwnd)
        finally:
            for thread in reversed(attached):
                api.AttachThreadInput(current, thread, False)
