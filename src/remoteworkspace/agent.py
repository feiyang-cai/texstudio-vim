"""Transient SSH workspace helper. Python 3 stdlib only; no listening socket.

The client sends framed JSON over SSH stdin/stdout. All filesystem operations
are confined to the explicitly selected project. No credentials are accepted.
"""
import atexit
import base64
import contextlib
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import queue
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time

VERSION = 1
MAX_FILE = 64 * 1024 * 1024
MAX_FRAME = 92 * 1024 * 1024
MAX_PROJECT = 1024 * 1024 * 1024
MAX_FILES = 100000
LOCK_NAME = '.texstudio-remote.lock'
_THREAD_LOCK = threading.Lock()
_CLOSED = threading.Event()
_ACTIVE = None


class RemoteError(Exception):
    pass


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def stop_child():
    global _ACTIVE
    child = _ACTIVE
    if child is None or child.poll() is not None:
        return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(child.pid, signal.SIGKILL)
    with contextlib.suppress(subprocess.TimeoutExpired):
        child.wait(timeout=5)


atexit.register(stop_child)


class Workspace:
    def __init__(self, root):
        if not isinstance(root, str) or not root or '\0' in root:
            raise RemoteError('Choose an absolute project folder.')
        root = os.path.expanduser(root)
        if not os.path.isabs(root):
            raise RemoteError('The project folder must be absolute on the remote machine.')
        self.root = Path(os.path.realpath(root))
        if not self.root.is_dir():
            raise RemoteError('The remote project folder does not exist.')
        if self.root == Path(self.root.anchor) or self.root == Path.home().resolve():
            raise RemoteError('Choose a paper project folder, not a drive root or your entire home folder.')

    def confined(self, name, directory=False):
        if not isinstance(name, str) or not name or '\\' in name or '\0' in name:
            raise RemoteError('Invalid project-relative filename.')
        parts = PurePosixPath(name).parts
        if not parts or name.startswith('/') or any(p in ('..', '.', '') or ':' in p for p in parts):
            raise RemoteError('Paths must stay inside the selected project.')
        if any(p == LOCK_NAME for p in parts):
            raise RemoteError('Reserved workspace filename.')
        lexical = self.root.joinpath(*parts)
        resolved = Path(os.path.realpath(lexical))
        try:
            resolved.relative_to(self.root)
        except ValueError:
            raise RemoteError('A symbolic link points outside the selected project: ' + name)
        if directory and not resolved.is_dir():
            raise RemoteError('Working directory does not exist.')
        return lexical, resolved

    @contextlib.contextmanager
    def lock(self):
        with _THREAD_LOCK:
            identity = str(getattr(os, 'getuid', lambda: os.environ.get('USERNAME', 'user'))())
            folder = Path(tempfile.gettempdir()) / ('texstudio-remote-' + identity)
            folder.mkdir(mode=0o700, exist_ok=True)
            if folder.is_symlink() or not folder.is_dir():
                raise RemoteError('Unsafe remote lock folder.')
            if os.name != 'nt' and (folder.stat().st_uid != os.getuid() or folder.stat().st_mode & 0o077):
                raise RemoteError('Remote lock folder is not private.')
            path = folder / (hashlib.sha256(os.fsencode(self.root)).hexdigest() + '.lock')
            if path.is_symlink():
                raise RemoteError('Unsafe workspace lock file.')
            flags = os.O_CREAT | os.O_RDWR | getattr(os, 'O_NOFOLLOW', 0)
            fd = os.open(path, flags, 0o600)
            stream = os.fdopen(fd, 'r+b')
            try:
                if path.is_symlink() or os.stat(path).st_ino != os.fstat(fd).st_ino:
                    raise RemoteError('Workspace lock file changed.')
                if os.name == 'nt':
                    import msvcrt
                    stream.write(b'0'); stream.flush(); stream.seek(0)
                    # Nonblocking retries also work for long server sessions.
                    deadline = time.monotonic() + 30
                    while True:
                        try:
                            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
                            break
                        except OSError:
                            if time.monotonic() >= deadline:
                                raise RemoteError('Another remote save is still running.')
                            time.sleep(.05)
                else:
                    import fcntl
                    fcntl.flock(fd, fcntl.LOCK_EX)
                yield
            finally:
                if os.name == 'nt':
                    import msvcrt
                    with contextlib.suppress(OSError):
                        stream.seek(0); msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
                stream.close()

    def listing(self):
        result = []
        total = 0

        def walk(directory, relative, ancestors):
            nonlocal total
            real = Path(os.path.realpath(directory))
            if real in ancestors:
                raise RemoteError('A project directory link forms a loop: ' + relative)
            try:
                real.relative_to(self.root)
            except ValueError:
                raise RemoteError('A project link leaves the selected folder: ' + relative)
            ancestors = ancestors | {real}
            for entry in sorted(os.scandir(directory), key=lambda e: e.name):
                if entry.name == LOCK_NAME or entry.name.startswith('.txs-save-'):
                    continue
                name = relative + entry.name
                _, target = self.confined(name)
                if target.is_dir():
                    walk(target, name + '/', ancestors)
                elif target.is_file():
                    info = target.stat()
                    if not stat.S_ISREG(info.st_mode):
                        continue
                    if info.st_size > MAX_FILE:
                        raise RemoteError('Project file exceeds 64 MiB: ' + name)
                    total += info.st_size
                    if total > MAX_PROJECT or len(result) >= MAX_FILES:
                        raise RemoteError('Project exceeds the workspace transfer limit.')
                    result.append({'path': name, 'size': info.st_size,
                                   'hash': digest(target), 'mode': stat.S_IMODE(info.st_mode)})
                else:
                    raise RemoteError('Unsupported or broken project link: ' + name)
        walk(self.root, '', set())
        return result

    def read(self, name):
        _, path = self.confined(name)
        with path.open('rb') as stream:
            data = stream.read(MAX_FILE + 1)
        if len(data) > MAX_FILE:
            raise RemoteError('Project file exceeds 64 MiB.')
        return {'data': base64.b64encode(data).decode('ascii'),
                'hash': hashlib.sha256(data).hexdigest(),
                'mode': stat.S_IMODE(path.stat().st_mode)}

    def put(self, name, data, expected):
        if not isinstance(data, str) or len(data) > (MAX_FILE + 2) // 3 * 4:
            raise RemoteError('Invalid file transfer.')
        try:
            data = base64.b64decode(data, validate=True)
        except (ValueError, TypeError):
            raise RemoteError('Invalid file transfer encoding.')
        if len(data) > MAX_FILE:
            raise RemoteError('Project file exceeds 64 MiB.')
        new_hash = hashlib.sha256(data).hexdigest()
        with self.lock():
            _, path = self.confined(name)
            if path.exists() and not path.is_file():
                raise RemoteError('Cannot replace a directory with a file.')
            actual = digest(path) if path.is_file() else None
            if actual == new_hash:
                return {'hash': new_hash}  # retry after a lost acknowledgement
            if actual != expected:
                raise RemoteError('Conflict: the remote file changed. Your local copy is preserved: ' + name)
            path.parent.mkdir(parents=True, exist_ok=True)
            _, checked = self.confined(name)
            if checked != path:
                raise RemoteError('The remote path changed during saving.')
            fd, temporary = tempfile.mkstemp(prefix='.txs-save-', dir=path.parent)
            try:
                with os.fdopen(fd, 'wb') as stream:
                    stream.write(data); stream.flush(); os.fsync(stream.fileno())
                if path.exists():
                    os.chmod(temporary, stat.S_IMODE(path.stat().st_mode))
                else:
                    os.chmod(temporary, 0o600)
                if (digest(path) if path.is_file() else None) != actual:
                    raise RemoteError('Conflict: the remote file changed during saving: ' + name)
                self.confined(name)
                os.replace(temporary, path)
            finally:
                with contextlib.suppress(FileNotFoundError):
                    os.unlink(temporary)
        return {'hash': new_hash}

    def delete(self, name, expected):
        with self.lock():
            lexical, path = self.confined(name)
            actual = digest(path) if path.is_file() else None
            if actual is None:
                return {}
            if actual != expected:
                raise RemoteError('Conflict: the remote file changed before deletion: ' + name)
            lexical.unlink()
        return {}

    def command(self, request, send, incoming):
        global _ACTIVE
        argv = request.get('argv')
        if not isinstance(argv, list) or not argv or not all(isinstance(v, str) and '\0' not in v for v in argv):
            raise RemoteError('Invalid remote command arguments.')
        cwd = request.get('cwd', '')
        directory = self.confined(cwd, directory=True)[1] if cwd else self.root
        environment = os.environ.copy()
        # TeX helpers may not be on the noninteractive SSH PATH.
        extras = ['/Library/TeX/texbin'] if sys.platform == 'darwin' else []
        import glob
        for tinytex in (Path.home()/'.TinyTeX', Path.home()/'Library'/'TinyTeX',
                        Path(environment.get('APPDATA', str(Path.home())))/'TinyTeX'):
            extras.extend(sorted(glob.glob(str(tinytex/'bin'/'*'))))
        if os.name == 'nt':
            for variable in ('LOCALAPPDATA', 'PROGRAMFILES'):
                if environment.get(variable):
                    extras.append(str(Path(environment[variable]) / 'Programs' / 'MiKTeX' / 'miktex' / 'bin' / 'x64'))
                    extras.append(str(Path(environment[variable]) / 'MiKTeX' / 'miktex' / 'bin' / 'x64'))
            extras.extend(sorted(glob.glob('C:/texlive/*/bin/windows'), reverse=True))
        environment['PATH'] = os.pathsep.join(extras + [environment.get('PATH', '')])
        for key, value in request.get('tex_paths', {}).items():
            if key not in ('TEXINPUTS', 'BIBINPUTS', 'BSTINPUTS', 'MFINPUTS', 'MPINPUTS', 'TFMFONTS'):
                raise RemoteError('Only TeX search paths may be forwarded.')
            if not isinstance(value, list) or not all(isinstance(p, str) for p in value):
                raise RemoteError('Invalid TeX search paths.')
            environment[key] = os.pathsep.join(value) + os.pathsep + environment.get(key, '')
        program = argv[0].replace('\\', '/').rsplit('/', 1)[-1].lower()
        stem = program[:-4] if program.endswith('.exe') else program
        tools = ('latex', 'pdflatex', 'xelatex', 'lualatex', 'latexmk', 'bibtex', 'bibtex8', 'biber',
                 'makeindex', 'texindy', 'xindy', 'makeglossaries', 'mpost', 'asy', 'dvipng', 'dvips',
                 'dvipdf', 'ps2pdf', 'gs', 'git', 'svn', 'texdoc')
        if stem in ('python', 'python3'):
            argv[0] = sys.executable
        elif stem in tools:
            argv[0] = shutil.which(stem, path=environment['PATH']) or stem
        flags = {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == 'nt' else {'start_new_session': True}
        child = subprocess.Popen(argv, cwd=directory, env=environment, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, **flags)
        _ACTIVE = child
        output = queue.Queue(maxsize=32)

        def pump(stream, channel):
            try:
                for data in iter(lambda: stream.read1(8192), b''):
                    output.put((channel, data))
            finally:
                output.put((channel, None))
        for channel, stream in (('stdout', child.stdout), ('stderr', child.stderr)):
            threading.Thread(target=pump, args=(stream, channel), daemon=True).start()
        ended = 0
        cancelled = False
        try:
            while ended < 2:
                if _CLOSED.is_set():
                    stop_child(); cancelled = True
                try:
                    control = incoming.get_nowait()
                except queue.Empty:
                    control = None
                if control is not None:
                    if control.get('op') != 'cancel':
                        raise RemoteError('Only cancellation may interrupt a running command.')
                    stop_child(); cancelled = True
                try:
                    channel, data = output.get(timeout=.05)
                except queue.Empty:
                    continue
                if data is None:
                    ended += 1
                else:
                    send({'event': channel, 'data': base64.b64encode(data).decode('ascii')})
            code = child.wait()
            return {'exit_code': 130 if cancelled else code}
        finally:
            stop_child()
            child.stdout.close(); child.stderr.close()
            _ACTIVE = None


def serve():
    incoming = queue.Queue(maxsize=8)
    workspace = None

    def reader():
        try:
            while True:
                line = sys.stdin.buffer.readline(MAX_FRAME + 1)
                if not line:
                    break
                if len(line) > MAX_FRAME:
                    incoming.put({'invalid': 'Request exceeds the transfer limit.'})
                    break
                try:
                    request = json.loads(line)
                    if not isinstance(request, dict):
                        raise ValueError('Expected an object')
                    incoming.put(request)
                except (ValueError, UnicodeError):
                    incoming.put({'invalid': 'Invalid protocol request.'})
        finally:
            _CLOSED.set()
            incoming.put(None)
    threading.Thread(target=reader, daemon=True).start()

    def send(message):
        message['protocol'] = VERSION
        try:
            sys.stdout.write(json.dumps(message, ensure_ascii=True, separators=(',', ':')) + '\n')
            sys.stdout.flush()
        except (BrokenPipeError, OSError):
            _CLOSED.set(); stop_child(); raise
    def stop_server(*_):
        _CLOSED.set(); stop_child()
        with contextlib.suppress(queue.Full): incoming.put_nowait(None)
    for signame in ('SIGTERM', 'SIGHUP'):
        sig = getattr(signal, signame, None)
        if sig is not None:
            signal.signal(sig, stop_server)
    while True:
        request = incoming.get()
        if request is None:
            break
        try:
            if 'invalid' in request:
                raise RemoteError(request['invalid'])
            op = request.get('op')
            if op == 'hello':
                workspace = Workspace(request.get('root'))
                response = {'root': str(workspace.root).replace('\\', '/'), 'platform': sys.platform,
                            'pathsep': os.pathsep, 'version': VERSION}
            elif workspace is None:
                raise RemoteError('Connect to a project before sending commands.')
            elif op == 'list':
                response = {'files': workspace.listing()}
            elif op == 'read':
                response = workspace.read(request.get('path'))
            elif op == 'put':
                if 'expected' not in request:
                    raise RemoteError('A remote revision is required before saving.')
                response = workspace.put(request.get('path'), request.get('data'), request['expected'])
            elif op == 'delete':
                if not isinstance(request.get('expected'), str):
                    raise RemoteError('A remote revision is required before deletion.')
                response = workspace.delete(request.get('path'), request['expected'])
            elif op == 'command':
                response = workspace.command(request, send, incoming)
            else:
                raise RemoteError('Unknown workspace operation.')
            send({'id': request.get('id'), 'ok': True, 'result': response})
        except (RemoteError, OSError, ValueError, TypeError) as error:
            send({'id': request.get('id'), 'ok': False, 'error': str(error)})
    stop_child()


if __name__ == '__main__':
    serve()
