"""Verify an immutable tag and its existing build before recovering publication."""
import json
import os
from pathlib import Path
import re
import subprocess
from urllib.error import HTTPError
from urllib.parse import quote
from urllib.request import Request, urlopen

repository = os.environ['GITHUB_REPOSITORY']
request = json.loads(Path('.github/release-request.json').read_text())
tag, sha, run_id = request['tag'], request['sha'], request['build_run_id']
if not re.fullmatch(r'texstudio-vim-[0-9]+\.[0-9]+\.[0-9]+(?:(?:alpha|beta|rc)[0-9]+)?-r[0-9]+', tag):
    raise SystemExit('Invalid fork release tag')
if not re.fullmatch(r'[0-9a-f]{40}', sha) or not isinstance(run_id, int):
    raise SystemExit('Invalid source build identity')

def get(path):
    query = Request(f'https://api.github.com/repos/{repository}' + path,
                    headers={'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
                             'Accept': 'application/vnd.github+json'})
    with urlopen(query, timeout=30) as response:
        return json.load(response)

reference = get('/git/ref/tags/' + quote(tag, safe=''))['object']
while reference['type'] == 'tag':
    reference = get('/git/tags/' + reference['sha'])['object']
if reference['type'] != 'commit' or reference['sha'] != sha:
    raise SystemExit('Release tag no longer points to the requested build commit')
run = get('/actions/runs/' + str(run_id))
if run['head_sha'] != sha or run['head_branch'] != tag or run['head_repository']['full_name'] != repository:
    raise SystemExit('Requested build does not belong to this immutable release tag')
required_jobs = {'linux appimage', 'win10 build (msys2)', 'win10 build (msys2,arm)',
                 'Mac OS X', 'Mac OS X (M1)', 'verify / Linux-autoTests'}
jobs = get(f'/actions/runs/{run_id}/jobs?per_page=100')['jobs']
if not required_jobs <= {job['name'] for job in jobs if job['conclusion'] == 'success'}:
    raise SystemExit('All platform builds and Linux regression checks must have passed')
artifacts = get(f'/actions/runs/{run_id}/artifacts?per_page=100')['artifacts']
required_artifacts = {'release-linux', 'release-win', 'release-win-arm', 'release-osx', 'release-osx-m1'}
if not required_artifacts <= {item['name'] for item in artifacts if not item['expired']}:
    raise SystemExit('Complete release payload is unavailable')
changed = subprocess.check_output(['git', 'diff', '--name-only', sha, 'HEAD'], text=True).splitlines()
if any(not (name.startswith('.github/') or name.endswith('.md')) for name in changed):
    raise SystemExit('Application changes require a new build and tag, not publication recovery')
try:
    release = get('/releases/tags/' + quote(tag, safe=''))
except HTTPError as error:
    if error.code != 404: raise
else:
    if not release['draft']:
        raise SystemExit('This tag is already published; published assets will not be replaced')
gui_required = True
verified_run_id = request.get('verified_gui_run_id')
if verified_run_id is not None:
    if not isinstance(verified_run_id, int):
        raise SystemExit('Invalid prior GUI verification run ID')
    verified = get('/actions/runs/' + str(verified_run_id))
    if verified['head_repository']['full_name'] != repository:
        raise SystemExit('GUI verification must belong to this repository')
    verified_request = json.loads(subprocess.check_output(
        ['git', 'show', verified['head_sha'] + ':.github/release-request.json'], text=True))
    if any(verified_request[key] != request[key] for key in ('tag', 'sha', 'build_run_id')):
        raise SystemExit('Prior GUI checks used a different release build')
    publisher_only = {'.github/scripts/publish-verified-release.py', '.github/scripts/read-release-request.py',
                      '.github/workflows/publish-verified-tag.yml', '.github/release-request.json'}
    subsequent = subprocess.check_output(['git', 'diff', '--name-only', verified['head_sha'], 'HEAD'], text=True).splitlines()
    if any(name not in publisher_only and not name.endswith('.md') for name in subsequent):
        raise SystemExit('GUI checker changes require rerunning all GUI environments')
    verified_jobs = get(f'/actions/runs/{verified_run_id}/jobs?per_page=100')['jobs']
    required_gui = {'gui / Packaged retest (' + runner + ')' for runner in
                    ('windows-latest', 'windows-11-arm', 'macos-14', 'macos-15-intel')}
    required_gui |= {'gui / AppImage retest (' + distro + ')' for distro in
                     ('ubuntu-22.04', 'ubuntu-24.04', 'debian-12', 'debian-13', 'fedora-43')}
    if not required_gui <= {job['name'] for job in verified_jobs if job['conclusion'] == 'success'}:
        raise SystemExit('All nine prior GUI checks must have passed')
    gui_required = False
    print(f'Reusing successful GUI verification {verified_run_id}; only publication code changed')
prerelease = bool(re.search(r'(alpha|beta|rc)[0-9]+-r', tag))
with Path(os.environ['GITHUB_OUTPUT']).open('a') as output:
    output.write(f'tag={tag}\nsha={sha}\nrun_id={run_id}\nprerelease={str(prerelease).lower()}\ngui_required={str(gui_required).lower()}\n')
print(f'Verified existing tag {tag}, commit {sha}, build {run_id}; application sources unchanged')
