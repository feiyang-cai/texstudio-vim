import argparse
import os
from pathlib import Path
import subprocess
from ssh_fixture import ssh_fixture

parser=argparse.ArgumentParser()
parser.add_argument('executable')
args=parser.parse_args()
with ssh_fixture() as fixture:
    environment=os.environ.copy()
    environment['TXS_TEST_SSH_CONFIG']=fixture['config']
    environment['TXS_TEST_REMOTE_ROOT']=fixture['root']
    environment['TXS_TEST_REMOTE_PYTHON']='python' if os.name=='nt' else 'python3'
    result=subprocess.run([str(Path(args.executable).resolve())],env=environment)
    raise SystemExit(result.returncode)
