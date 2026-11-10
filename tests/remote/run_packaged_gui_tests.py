#!/usr/bin/env python3
"""Test SSH saves and builds through the shipped AppImage and its real launcher."""
import argparse
import base64
import gzip
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import uuid
from ssh_fixture import ssh_fixture

parser = argparse.ArgumentParser()
parser.add_argument('appimage')
parser.add_argument('--output', default='packaged-remote-results')
args = parser.parse_args()
image = Path(args.appimage).resolve(); image.chmod(image.stat().st_mode | 0o100)
output = Path(args.output).resolve(); output.mkdir(parents=True, exist_ok=True)
command = [str(image), '--appimage-extract-and-run']
environment = os.environ.copy()
environment.update(XDG_DATA_HOME=str(output/'data'), XDG_CACHE_HOME=str(output/'cache'),
                   QT_LOGGING_TO_CONSOLE='1', QT_FORCE_STDERR_LOGGING='1')

def wait_for(test, description, timeout=90):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if test(): return
        time.sleep(.2)
    raise RuntimeError(description)

def xdo(*arguments):
    result = subprocess.run(['xdotool', *arguments], capture_output=True, text=True, timeout=30)
    if result.returncode: raise RuntimeError('xdotool failed: '+result.stderr.strip())
    return result.stdout

with ssh_fixture() as fixture:
    folder = output/'data'/'TeXstudio'/'remote-workspaces'/str(uuid.uuid4())
    folder.mkdir(parents=True)
    manifest = folder/'profile.json'
    manifest.write_text(json.dumps(dict(version=1,host='office-test',root=fixture['root'],python='python3',
                                       ssh_config=fixture['config'],platform='',ready=False)))
    # Initialize a real connection through AppRun. The GUI then opens this cache,
    # and must launch its own background workers for save and build below.
    connect = subprocess.run(command + ['--remote-workspace-worker',str(manifest),'connect',
                             base64.b64encode(b'{}').decode()], env=environment,
                             capture_output=True, text=True, timeout=120)
    (output/'connect.log').write_text(connect.stdout+connect.stderr)
    if connect.returncode: raise RuntimeError('Packaged SSH connection failed; see connect.log')
    source = folder/'project'/'main.tex'
    remote_source = Path(fixture['root'])/'main.tex'
    config = output/'config'; config.mkdir(exist_ok=True)
    (config/'texstudio.ini').write_text('[texmaker]\nStartup\\CheckLatexConfiguration=false\n')
    with (output/'desktop.log').open('w') as log:
        process = subprocess.Popen(command+['--start-always','--disable-tests','--config',str(config),str(source)],
                                   env=environment,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        try:
            window = xdo('search','--sync','--onlyvisible','--name','main[.]tex.*TeXstudio').splitlines()[0]
            xdo('windowactivate','--sync',window)
            time.sleep(1)
            geometry = dict(line.split('=',1) for line in xdo('getwindowgeometry','--shell',window).splitlines() if '=' in line)
            xdo('mousemove','--window',window,str(int(geometry['WIDTH'])*2//3),str(int(geometry['HEIGHT'])//4),'click','1')
            xdo('key','--clearmodifiers','Escape')
            xdo('type','--clearmodifiers','--delay','60','ggO% AppImage SSH save')
            xdo('key','--clearmodifiers','Escape')
            xdo('type','--clearmodifiers','--delay','60',':w')
            xdo('key','Return')
            marker = b'% AppImage SSH save'
            wait_for(lambda: marker in source.read_bytes(), 'The native Vim save did not update the cache')
            wait_for(lambda: marker in remote_source.read_bytes(), 'The GUI failed to start its packaged SSH save worker')
            (output/'saved-source.tex').write_bytes(source.read_bytes())
            # The build path has a separate worker start site from background saves.
            xdo('key','--clearmodifiers','F5')
            pdf = source.with_suffix('.pdf'); sync = source.with_suffix('.synctex.gz')
            wait_for(lambda: pdf.exists() and sync.exists(), 'The packaged remote build did not download PDF/SyncTeX outputs',120)
            if not pdf.read_bytes().startswith(b'%PDF-'): raise RuntimeError('Downloaded output is not a PDF')
            remote_pdf = remote_source.with_suffix('.pdf')
            if pdf.read_bytes()!=remote_pdf.read_bytes(): raise RuntimeError('Downloaded PDF differs from the remote output')
            if sync.read_bytes()!=remote_source.with_suffix('.synctex.gz').read_bytes(): raise RuntimeError('SyncTeX bytes changed in transfer')
            if b'main.tex' not in gzip.decompress(sync.read_bytes()): raise RuntimeError('Downloaded SyncTeX has no source mapping')
            time.sleep(1)
            (output/'result.txt').write_text('PASS: packaged AppImage, custom SSH config, Vim save, GUI worker launch, remote LaTeX build, PDF and SyncTeX transfer.\n')
            print((output/'result.txt').read_text())
        finally:
            subprocess.run(['scrot',str(output/'desktop.png')],check=False)
            if process.poll() is None:
                os.killpg(process.pid,signal.SIGKILL);process.wait()
