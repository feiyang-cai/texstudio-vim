import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib.util
import json
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
import unittest

AGENT = Path(__file__).resolve().parents[2] / 'src/remoteworkspace/agent.py'
spec = importlib.util.spec_from_file_location('remote_agent', AGENT)
agent = importlib.util.module_from_spec(spec)
spec.loader.exec_module(agent)


def encoded(data): return base64.b64encode(data).decode('ascii')
def sha(data): return hashlib.sha256(data).hexdigest()


class AgentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='paper space ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.ws = agent.Workspace(str(self.root))

    def test_binary_unicode_and_spaces_roundtrip(self):
        data = 'αβ 中文'.encode() + b'\0\xff\r\n'
        name = "chapter 'quoted'; $name/中文.tex"
        self.ws.put(name, encoded(data), None)
        result = self.ws.read(name)
        self.assertEqual(base64.b64decode(result['data']), data)
        self.assertEqual(result['hash'], sha(data))
        self.assertEqual(self.ws.listing()[0]['path'], name)

    def test_conflict_preserves_remote_changes(self):
        path = self.root / 'paper.tex'
        path.write_bytes(b'office change')
        with self.assertRaisesRegex(agent.RemoteError, 'Conflict'):
            self.ws.put('paper.tex', encoded(b'home change'), sha(b'old'))
        self.assertEqual(path.read_bytes(), b'office change')

    def test_lost_acknowledgement_can_be_retried(self):
        self.ws.put('paper.tex', encoded(b'new'), None)
        self.assertEqual(self.ws.put('paper.tex', encoded(b'new'), None)['hash'], sha(b'new'))

    def test_concurrent_saves_only_one_revision_wins(self):
        self.ws.put('paper.tex', encoded(b'original'), None)
        def save(data):
            try:
                self.ws.put('paper.tex', encoded(data), sha(b'original'))
                return True
            except agent.RemoteError:
                return False
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(save, (b'one', b'two')))
        self.assertEqual(sum(results), 1)
        self.assertIn((self.root / 'paper.tex').read_bytes(), (b'one', b'two'))

    def test_no_escape_or_reserved_paths(self):
        for name in ('../escape.tex', '/tmp/escape', 'C:/escape', 'a/../../escape', r'a\..\escape', '.', agent.LOCK_NAME):
            with self.subTest(name=name), self.assertRaises(agent.RemoteError):
                self.ws.put(name, encoded(b'bad'), None)

    def test_invalid_transfer_preserves_file(self):
        self.ws.put('paper.tex', encoded(b'good'), None)
        with self.assertRaises(agent.RemoteError):
            self.ws.put('paper.tex', 'not-base64!', sha(b'good'))
        self.assertEqual((self.root / 'paper.tex').read_bytes(), b'good')

    def test_delete_checks_revision(self):
        self.ws.put('paper.tex', encoded(b'good'), None)
        with self.assertRaises(agent.RemoteError):
            self.ws.delete('paper.tex', sha(b'wrong'))
        self.assertTrue((self.root / 'paper.tex').exists())
        self.ws.delete('paper.tex', sha(b'good'))
        self.assertFalse((self.root / 'paper.tex').exists())

    def test_cannot_replace_directory(self):
        (self.root / 'folder').mkdir()
        with self.assertRaises(agent.RemoteError):
            self.ws.put('folder', encoded(b'bad'), None)
        self.assertTrue((self.root / 'folder').is_dir())

    def test_symbolic_link_stays_confined_and_is_preserved(self):
        target = self.root / 'paper.tex'; target.write_bytes(b'old')
        link = self.root / 'alias.tex'
        try:
            link.symlink_to(target)
        except (OSError, NotImplementedError):
            return  # Windows may not permit symlink creation for this account.
        self.ws.put('alias.tex', encoded(b'new'), sha(b'old'))
        self.assertTrue(link.is_symlink())
        self.assertEqual(target.read_bytes(), b'new')
        self.ws.delete('alias.tex', sha(b'new'))
        self.assertTrue(target.exists())
        link.symlink_to(self.root.parent / 'outside.tex')
        with self.assertRaises(agent.RemoteError):
            self.ws.put('alias.tex', encoded(b'bad'), None)

    def test_directory_link_loop_is_rejected(self):
        try:
            (self.root / 'cycle').symlink_to(self.root, target_is_directory=True)
        except (OSError, NotImplementedError):
            return
        with self.assertRaisesRegex(agent.RemoteError, 'loop'):
            self.ws.listing()

    def test_home_and_drive_roots_are_not_project_folders(self):
        for path in (str(Path.home()), str(self.root.anchor)):
            with self.assertRaises(agent.RemoteError):
                agent.Workspace(path)


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='rpc paper ')
        self.addCleanup(self.temp.cleanup)
        self.process = subprocess.Popen([sys.executable, '-u', str(AGENT)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.cleanup)
        self.replies = queue.Queue()
        def read():
            for line in self.process.stdout:
                self.replies.put(json.loads(line))
        threading.Thread(target=read, daemon=True).start()
        self.next_id = 0
        self.assertTrue(self.request('hello', root=self.temp.name)['ok'])

    def cleanup(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try: self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill(); self.process.wait()
        for stream in (self.process.stdout, self.process.stderr): stream.close()

    def send(self, op, **args):
        self.next_id += 1
        self.process.stdin.write((json.dumps(dict(args, op=op, id=self.next_id))+'\n').encode())
        self.process.stdin.flush()
        return self.next_id

    def request(self, op, **args):
        request_id = self.send(op, **args)
        response = self.replies.get(timeout=10)
        self.assertEqual(response['protocol'], 1)
        self.assertEqual(response['id'], request_id)
        return response

    def test_roundtrip_and_errors_are_structured(self):
        self.assertTrue(self.request('put', path='a.tex', data=encoded(b'paper'), expected=None)['ok'])
        self.assertEqual(self.request('list')['result']['files'][0]['path'], 'a.tex')
        self.assertFalse(self.request('put', path='../outside', data=encoded(b'bad'), expected=None)['ok'])
        self.assertFalse(self.request('put', path='a.tex', data=encoded(b'lost'))['ok'])

    def test_remote_command_streams_both_channels_and_returns_status(self):
        request_id = self.send('command', argv=[sys.executable, '-c',
            "import sys;print('hello');print('error',file=sys.stderr);sys.exit(7)"])
        output = []
        while True:
            message = self.replies.get(timeout=10)
            if message.get('id') == request_id:
                self.assertEqual(message['result']['exit_code'], 7); break
            output.append((message['event'], base64.b64decode(message['data'])))
        self.assertIn(('stdout', b'hello\n' if sys.platform != 'win32' else b'hello\r\n'), output)
        self.assertTrue(any(channel == 'stderr' and b'error' in data for channel, data in output))

    def test_cancel_terminates_the_remote_command(self):
        request_id = self.send('command', argv=[sys.executable, '-u', '-c',
            "import time;print('started',flush=True);time.sleep(60)"])
        started = self.replies.get(timeout=10)
        self.assertEqual(started['event'], 'stdout')
        self.send('cancel')
        while True:
            message = self.replies.get(timeout=10)
            if message.get('id') == request_id:
                self.assertEqual(message['result']['exit_code'], 130); break


if __name__ == '__main__': unittest.main()
