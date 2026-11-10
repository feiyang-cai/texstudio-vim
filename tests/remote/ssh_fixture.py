"""Disposable loopback-only SSH server for integration tests; no user SSH edits."""
import contextlib
import getpass
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time


@contextlib.contextmanager
def ssh_fixture():
    server = os.environ.get('TXS_TEST_SSHD') or shutil.which('sshd')
    if not server:
        raise RuntimeError('An OpenSSH server executable is required for the integration fixture.')
    with tempfile.TemporaryDirectory(prefix='texstudio ssh fixture ') as temporary:
        folder = Path(temporary)
        if os.name == 'nt':
            # Win32-OpenSSH checks ACLs even on a disposable, non-service server.
            subprocess.run(['icacls', str(folder), '/inheritance:r', '/grant:r',
                            getpass.getuser()+':(OI)(CI)F', 'SYSTEM:(OI)(CI)F'],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for name in ('host_key', 'client_key'):
            subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(folder/name)], check=True,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        public = (folder/'client_key.pub').read_text()
        (folder/'authorized_keys').write_text(public)
        (folder/'authorized_keys').chmod(0o600)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0)); port = probe.getsockname()[1]
        def quoted(path): return '"' + Path(path).as_posix() + '"'
        options = [f'Port {port}', 'ListenAddress 127.0.0.1',
                   'HostKey '+quoted(folder/'host_key'), 'PidFile '+quoted(folder/'sshd.pid'),
                   'AuthorizedKeysFile '+quoted(folder/'authorized_keys'), 'StrictModes no',
                   'PasswordAuthentication no', 'KbdInteractiveAuthentication no',
                   'PubkeyAuthentication yes', 'AuthenticationMethods publickey',
                   'AllowUsers '+getpass.getuser(), 'LogLevel VERBOSE']
        if os.name != 'nt': options += ['UsePAM no']
        (folder/'sshd_config').write_text('\n'.join(options)+'\n')
        output = (folder/'server.log').open('wb')
        process = subprocess.Popen([server, '-D', '-e', '-f', str(folder/'sshd_config')], stdout=output, stderr=output)
        try:
            deadline = time.monotonic()+20
            while True:
                if process.poll() is not None:
                    output.flush()
                    raise RuntimeError('Disposable SSH server stopped: '+(folder/'server.log').read_text(errors='replace'))
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.2): break
                except OSError:
                    if time.monotonic()>deadline: raise RuntimeError('Disposable SSH server did not start.')
                    time.sleep(.1)
            key_fields=(folder/'host_key.pub').read_text().split()
            (folder/'known_hosts').write_text('office-fixture '+key_fields[0]+' '+key_fields[1]+'\n')
            config = ['Host office-test', '  HostName 127.0.0.1', f'  Port {port}', '  User '+getpass.getuser(),
                      '  IdentityFile '+quoted(folder/'client_key'), '  UserKnownHostsFile '+quoted(folder/'known_hosts'),
                      '  HostKeyAlias office-fixture', '  IdentitiesOnly yes', '  BatchMode yes', '  StrictHostKeyChecking yes']
            config += ['  RemoteCommand exit 99']  # Workspace transport must override interactive login commands.
            (folder/'config').write_text('\n'.join(config)+'\n')
            paper = folder/'office paper'; paper.mkdir()
            (paper/'main.tex').write_text('\\documentclass{article}\n\\begin{document}Office revision.\\end{document}\n')
            yield {'config': str(folder/'config'), 'root': str(paper), 'folder': str(folder)}
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
            output.close()
