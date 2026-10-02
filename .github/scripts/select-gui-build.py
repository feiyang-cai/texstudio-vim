#!/usr/bin/env python3
"""Find existing packages for a GUI-only retest, without testing stale app code."""
import json
import os
from pathlib import Path
import re
from urllib.parse import quote
from urllib.request import Request, urlopen

repository = os.environ['GITHUB_REPOSITORY']
base = f'https://api.github.com/repos/{repository}'

def get(path):
    request = Request(base + path, headers={'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
                                           'Accept': 'application/vnd.github+json'})
    with urlopen(request, timeout=30) as response:
        return json.load(response)

requested = os.environ.get('REQUESTED_BUILD_RUN_ID', '')
if requested:
    if not requested.isdigit():
        raise SystemExit('Build run ID must be numeric')
    candidates = [get('/actions/runs/' + requested)]
else:
    branch = quote(os.environ['GITHUB_REF_NAME'], safe='')
    candidates = get(f'/actions/workflows/cd.yml/runs?event=push&branch={branch}&per_page=20')['workflow_runs']

required = {'texstudio-linux', 'texstudio-vim-win-qt6-zip', 'texstudio-vim-win-arm-qt6-zip',
            'texstudio-osx', 'texstudio-vim-osx-m1'}
for run in candidates:
    sha = run['head_sha']
    if not re.fullmatch('[0-9a-f]{40}', sha):
        continue
    artifacts = get(f"/actions/runs/{run['id']}/artifacts?per_page=100")['artifacts']
    names = {item['name'] for item in artifacts if not item['expired']}
    if not required <= names:
        continue
    # Only reuse binaries when subsequent commits changed CI/docs exclusively.
    comparison = get(f"/compare/{sha}...{os.environ['GITHUB_SHA']}")
    files = comparison.get('files', [])
    if len(files) >= 300 or any(not (item['filename'].startswith('.github/') or item['filename'].endswith('.md'))
                               for item in files):
        continue
    with Path(os.environ['GITHUB_OUTPUT']).open('a') as output:
        output.write(f"run_id={run['id']}\nsha={sha}\n")
    print(f"Retesting package build {run['id']} at {sha}; application sources are unchanged")
    break
else:
    raise SystemExit('No complete package build with unchanged application sources; wait for CD and rerun')
