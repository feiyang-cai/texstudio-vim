"""Publish the explicitly owner-requested tag after its complete build/GUI gates.

This runs only on the isolated publication branch. The source tag and normal
automatic-publication policy/workflows remain unchanged.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

CD_JOBS = {
    'verify / Linux-autoTests', 'linux appimage', 'win10 build (msys2)',
    'win10 build (msys2,arm)', 'Mac OS X', 'Mac OS X (M1)',
    *('AppImage desktop (' + name + ')' for name in
      ('ubuntu-22.04', 'ubuntu-24.04', 'debian-12', 'debian-13', 'fedora-43')),
    *('Packaged GUI (' + name + ')' for name in
      ('windows-latest', 'windows-11-arm', 'macos-14', 'macos-15-intel')),
}
VIM_JOBS = {'Vim desktop (' + name + ')' for name in
            ('windows-latest', 'windows-11-arm', 'macos-14', 'macos-15-intel')}
PAYLOAD = {
    'texstudio-linux': ('*.AppImage', '-x86_64.AppImage'),
    'texstudio-vim-win-qt6-exe': ('*.exe', '-win-qt6.exe'),
    'texstudio-vim-win-qt6-zip': ('*.zip', '-win-portable-qt6.zip'),
    'texstudio-vim-win-arm-qt6-exe': ('*.exe', '-win-arm-qt6.exe'),
    'texstudio-vim-win-arm-qt6-zip': ('*.zip', '-win-arm-portable-qt6.zip'),
    'texstudio-osx': ('*.zip', '-osx.zip'),
    'texstudio-vim-osx-m1': ('*.zip', '-osx-m1.zip'),
}


def gh(*arguments, optional=False):
    result = subprocess.run(['gh', *arguments], text=True, capture_output=True)
    if result.returncode:
        if optional and 'HTTP 404' in result.stderr:
            return None
        raise RuntimeError(result.stderr.strip())
    return result.stdout


def api(path, optional=False):
    result = gh('api', path, optional=optional)
    return None if result is None else json.loads(result)


def validate_request(request):
    if (request['repository'] != 'feiyang-cai/texstudio-vim'
            or request['tag'] != 'texstudio-vim-4.9.9rc1-r1.1'
            or request['vim_revision'] != '1.1'
            or request['upstream_tag'] != '4.9.9rc1'
            or not re.fullmatch('[0-9a-f]{40}', request['sha'])
            or not re.fullmatch('[0-9a-f]{40}', request['upstream_sha'])):
        raise RuntimeError('Unexpected owner publication request')


def tag_commit(base, tag):
    obj = api(base + '/git/ref/tags/' + tag)['object']
    while obj['type'] == 'tag':
        obj = api(base + '/git/tags/' + obj['sha'])['object']
    if obj['type'] != 'commit':
        raise RuntimeError('Tag does not resolve to a source commit')
    return obj['sha']


def verify_jobs(run, jobs, required, sha, tag):
    if (run['head_sha'] != sha or run['head_branch'] != tag
            or run['head_repository']['full_name'] != 'feiyang-cai/texstudio-vim'
            or run['event'] != 'push' or run['conclusion'] != 'success'):
        raise RuntimeError('Release run has incorrect provenance or failed')
    passed = {job['name'] for job in jobs if job['conclusion'] == 'success'}
    if not required <= passed:
        raise RuntimeError('Missing successful release checks: ' + ', '.join(sorted(required - passed)))


def wait_for_run(base, workflow, request, required):
    deadline = time.monotonic() + 70 * 60
    while time.monotonic() < deadline:
        runs = api(base + '/actions/workflows/' + workflow + '/runs?event=push&branch='
                   + request['tag'] + '&per_page=30')['workflow_runs']
        matches = [run for run in runs if run['head_sha'] == request['sha']]
        if matches:
            run = max(matches, key=lambda item: item['id'])
            if run['status'] == 'completed':
                jobs = api(base + f'/actions/runs/{run["id"]}/jobs?per_page=100')['jobs']
                verify_jobs(run, jobs, required, request['sha'], request['tag'])
                print(f'Verified {workflow} run {run["id"]}: {len(required)} required checks', flush=True)
                return run
        time.sleep(30)
    raise RuntimeError('Timed out waiting for exact-tag release checks')


def verify_assets(release, expected, tag):
    if release['tag_name'] != tag or not release['prerelease']:
        raise RuntimeError('Unexpected release identity or channel')
    assets = release['assets']
    if len(assets) != len(expected) or {a['name'] for a in assets} != set(expected):
        raise RuntimeError('Incomplete or unexpected release asset set')
    for asset in assets:
        size, digest = expected[asset['name']]
        if (asset['state'] != 'uploaded' or asset['size'] != size
                or asset.get('digest') not in (None, 'sha256:' + digest)):
            raise RuntimeError('Uploaded asset verification failed: ' + asset['name'])


def release_for_tag(base, tag):
    # Listing with this write-scoped token includes drafts. Do not depend on
    # the public-by-tag endpoint finding a draft before it is published.
    matches = [release for release in api(base + '/releases?per_page=100')
               if release['tag_name'] == tag]
    if len(matches) > 1:
        raise RuntimeError('Ambiguous releases for the requested tag')
    return matches[0] if matches else None


def main():
    request = json.loads(Path('.github/manual-release-request.json').read_text())
    validate_request(request)
    if os.environ.get('GITHUB_ACTOR') != 'feiyang-cai':
        raise RuntimeError('This publication requires the owner-requested workflow run')
    repository, tag, sha = request['repository'], request['tag'], request['sha']
    base = 'repos/' + repository
    if tag_commit(base, tag) != sha:
        raise RuntimeError('Immutable release tag changed')
    subprocess.run(['git', 'merge-base', '--is-ancestor', request['upstream_sha'], sha], check=True)
    revision = subprocess.check_output(['git', 'show', sha + ':VIM_REVISION'], text=True).strip()
    if revision != request['vim_revision']:
        raise RuntimeError('Tagged source has an unexpected Vim revision')
    cd = wait_for_run(base, 'cd.yml', request, CD_JOBS)
    vim = wait_for_run(base, 'vim-platforms.yml', request, VIM_JOBS)
    artifacts = api(base + f'/actions/runs/{cd["id"]}/artifacts?per_page=100')['artifacts']
    payload = Path('owner-release-payload')
    payload.mkdir(exist_ok=True)
    for name, (pattern, suffix) in PAYLOAD.items():
        matching = [a for a in artifacts if a['name'] == name and not a['expired']]
        if len(matching) != 1:
            raise RuntimeError('Missing or ambiguous build artifact: ' + name)
        directory = Path('owner-release-downloads') / name
        gh('run', 'download', str(cd['id']), '--repo', repository, '--name', name, '--dir', str(directory))
        files = list(directory.glob(pattern))
        if len(files) != 1 or not files[0].is_file() or files[0].stat().st_size == 0:
            raise RuntimeError('Unexpected packaged artifact payload: ' + name)
        shutil.copyfile(files[0], payload / (tag + suffix))
    packages = sorted(payload.iterdir())
    if len(packages) != 7:
        raise RuntimeError('Expected seven platform packages')
    with (payload / 'hashes.txt').open('w') as hashes:
        for path in packages:
            with path.open('rb') as stream:
                digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            hashes.write(f'{digest}  {path.name}\n')
    expected = {}
    for path in payload.iterdir():
        with path.open('rb') as stream:
            expected[path.name] = (path.stat().st_size, hashlib.file_digest(stream, 'sha256').hexdigest())
    existing = release_for_tag(base, tag)
    if existing and not existing['draft']:
        verify_assets(existing, expected, tag)
        print('Verified already-published release: ' + existing['html_url'], flush=True)
        return
    notes = Path('owner-release-notes.md')
    notes.write_text(f'''TeXstudio **4.9.9rc1** with Vim **r1.1**. This is an experimental prerelease.

### Vim changes
- Mouse dragging in Normal mode enters Visual mode and preserves the exact selection; double-click word selection also enters Visual mode.
- Fixes selection boundaries, Unicode/folded text, Visual indentation, and stale selections after Ctrl/right-click or context menus. Ctrl-click navigation and native Insert/Replace selection remain available.
- Supports numeric Vim patch revisions in version comparisons, update checks and package names.

**Upgrade from r1:** download and install this version manually. Older r1 update checkers cannot recognize dotted Vim revisions. Saved editing preferences are preserved; Vim is the default for fresh configurations.

The immutable tag passed all platform builds, four native Vim desktop jobs and nine packaged GUI environments before publication. SHA-256 checksums are included in `hashes.txt`.

Changes are largely generated by AI coding agents with human guidance and testing. Vim support remains experimental and incomplete; keep backups of important work.

[Vim changelog](https://github.com/{repository}/blob/{tag}/VIM_CHANGELOG.md) · [Supported commands and limitations](https://github.com/{repository}/blob/{tag}/VIM_MODE.md) · [Issues and feedback](https://github.com/{repository}/issues)
''')
    if not existing:
        gh('release', 'create', tag, '--repo', repository, '--verify-tag', '--draft',
           '--prerelease', '--title', tag, '--notes-file', str(notes))
    elif any(a['name'] not in expected for a in existing['assets']):
        raise RuntimeError('Existing draft contains unexpected assets')
    gh('release', 'upload', tag, *map(str, sorted(payload.iterdir())), '--repo', repository, '--clobber')
    release = release_for_tag(base, tag)
    if release is None:
        raise RuntimeError('Uploaded draft release could not be found')
    verify_assets(release, expected, tag)
    if tag_commit(base, tag) != sha:
        raise RuntimeError('Tag changed before final publication')
    gh('release', 'edit', tag, '--repo', repository, '--draft=false', '--prerelease', '--latest=false',
       '--notes-file', str(notes))
    published = release_for_tag(base, tag)
    if published is None:
        raise RuntimeError('Published release could not be found')
    verify_assets(published, expected, tag)
    if published['draft']:
        raise RuntimeError('Release is still a draft')
    Path('publication-evidence.json').write_text(json.dumps({
        'tag': tag, 'sha': sha, 'cd_run_id': cd['id'], 'vim_run_id': vim['id'],
        'release_url': published['html_url'], 'assets': expected,
    }, indent=2) + '\n')
    print('Published complete verified prerelease: ' + published['html_url'], flush=True)


if __name__ == '__main__':
    main()
