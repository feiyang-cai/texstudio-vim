import argparse
import os
from pathlib import Path
import subprocess
import contextlib
import re
import shlex
import shutil
import sys
import tempfile
from ssh_fixture import ssh_fixture

parser=argparse.ArgumentParser()
parser.add_argument('executable')
parser.add_argument('--through-loader', action='store_true', help='Reproduce the standalone AppImage launcher on Linux')
args=parser.parse_args()
with contextlib.ExitStack() as stack:
    fixture=stack.enter_context(ssh_fixture())
    environment=os.environ.copy()
    environment['TXS_TEST_SSH_CONFIG']=fixture['config']
    environment['TXS_TEST_REMOTE_ROOT']=fixture['root']
    environment['TXS_TEST_REMOTE_PYTHON']='python' if os.name=='nt' else 'python3'
    executable=Path(args.executable).resolve()
    if args.through_loader:
        if sys.platform!='linux': raise SystemExit('--through-loader requires Linux')
        description=subprocess.check_output(['readelf','-l',str(executable)],text=True)
        match=re.search(r'Requesting program interpreter:\s*([^\]]+)',description)
        if not match: raise SystemExit('Cannot find the ELF interpreter')
        folder=Path(stack.enter_context(tempfile.TemporaryDirectory(prefix='texstudio loader test ')))
        target=folder/'usr/bin/remote-workspace-tests'; target.parent.mkdir(parents=True)
        shutil.copy2(executable,target)
        launcher=folder/'AppRun'
        launcher.write_text('#!/bin/sh\nexec '+shlex.quote(match[1])+' '+shlex.quote(str(target))+' "$@"\n')
        launcher.chmod(0o700)
        environment['APPDIR']=str(folder)
        executable=launcher
    result=subprocess.run([str(executable)],env=environment)
    raise SystemExit(result.returncode)
