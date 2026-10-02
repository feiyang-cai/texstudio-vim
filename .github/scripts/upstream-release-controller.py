#!/usr/bin/env python3
"""Verify upstream-sync checks and safely promote tested syncs to fork releases."""
import base64
import json
import os
from pathlib import Path
import re
import subprocess
import time
from urllib.parse import quote
from urllib.request import Request, urlopen

API = 'https://api.github.com'
UPSTREAM = 'texstudio-org/texstudio'
MARKER = re.compile(r'<!-- upstream-release-sync:(stable|prerelease):([0-9]+\.[0-9]+\.[0-9]+(?:(?:alpha|beta|rc)[0-9]+)?):([0-9a-f]{40}) -->')
RELEASE_VERSION = re.compile(r'[0-9]+\.[0-9]+\.[0-9]+(?:(?:alpha|beta|rc)[0-9]+)?')
PROTECTED_PATHS = {
    'VIM_REVISION',
    '.github/approved-vim-baseline.json',
    '.github/upstream-release-state.json',
    '.github/release-request.json',
    'RELEASES.md',
    '.github/workflows/ci.yml',
    '.github/workflows/cd.yml',
    '.github/workflows/vim-platforms.yml',
    '.github/workflows/packaged-gui.yml',
    '.github/workflows/publish-verified-tag.yml',
    '.github/workflows/upstream-release-sync.yml',
    '.github/scripts/detect-upstream-releases.py',
    '.github/scripts/upstream-release-controller.py',
    '.github/scripts/get-version.sh',
    '.github/scripts/read-release-request.py',
    '.github/scripts/publish-verified-release.py',
    '.github/scripts/select-gui-build.py',
}
CD_JOBS = {
    'verify / Linux-autoTests',
    'win10 build (msys2)',
    'win10 build (msys2,arm)',
    'linux appimage',
    'Mac OS X',
    'Mac OS X (M1)',
    'Packaged GUI (windows-latest)',
    'Packaged GUI (windows-11-arm)',
    'Packaged GUI (macos-15-intel)',
    'Packaged GUI (macos-14)',
    'AppImage desktop (ubuntu-22.04)',
    'AppImage desktop (ubuntu-24.04)',
    'AppImage desktop (debian-12)',
    'AppImage desktop (debian-13)',
    'AppImage desktop (fedora-43)',
}
VIM_JOBS = {
    'Vim desktop (windows-latest)',
    'Vim desktop (windows-11-arm)',
    'Vim desktop (macos-15-intel)',
    'Vim desktop (macos-14)',
}
FORK_RELEASE_TAG = re.compile(r'texstudio-vim-(' + RELEASE_VERSION.pattern + r')-r([0-9]+)')

repository = os.environ.get('GITHUB_REPOSITORY', 'feiyang-cai/texstudio-vim')
owner, repo = repository.split('/', 1)
token = os.environ.get('GH_TOKEN', '')


def api(method, path, payload=None, token_value=None):
    data = json.dumps(payload).encode() if payload is not None else None
    request = Request(API + path, data=data, method=method, headers={
        'Authorization': 'Bearer ' + (token_value or token),
        'Accept': 'application/vnd.github+json',
        'X-GitHub-Api-Version': '2022-11-28',
        'Content-Type': 'application/json',
    })
    with urlopen(request, timeout=30) as response:
        if response.status == 204:
            return None
        return json.load(response)


def branch_sha(branch):
    return api('GET', f'/repos/{repository}/git/ref/heads/{quote(branch, safe="")}')['object']['sha']


def marker_from(body):
    match = MARKER.search(body or '')
    return (match.group(1), match.group(2), match.group(3)) if match else None


def resolve_upstream_tag(tag):
    obj = api('GET', f'/repos/{UPSTREAM}/git/ref/tags/{quote(tag, safe="")}')['object']
    while obj['type'] == 'tag':
        obj = api('GET', f'/repos/{UPSTREAM}/git/tags/{obj["sha"]}')['object']
    if obj['type'] != 'commit':
        raise RuntimeError(f'Upstream tag {tag} no longer resolves to a commit')
    return obj['sha']


def newest_run(workflow, sha, events):
    runs = api('GET', f'/repos/{repository}/actions/workflows/{workflow}/runs'
                f'?head_sha={sha}&per_page=100')['workflow_runs']
    candidates = [run for run in runs if run['head_sha'] == sha and run['event'] in events]
    if not candidates:
        return None
    return max(candidates, key=lambda run: (run['run_number'], run['run_attempt']))


def require_job_set(run, required):
    if not run or run['status'] != 'completed' or run['conclusion'] != 'success':
        return False
    jobs = api('GET', f"/repos/{repository}/actions/runs/{run['id']}/jobs?per_page=100")['jobs']
    successes = {job['name'] for job in jobs if job['conclusion'] == 'success'}
    return required <= successes


def workflows_pass(sha, events):
    cd = newest_run('cd.yml', sha, events)
    vim = newest_run('vim-platforms.yml', sha, events)
    if not require_job_set(cd, CD_JOBS) or not require_job_set(vim, VIM_JOBS):
        return False
    return cd, vim


def pr_files(number):
    result = []
    page = 1
    while True:
        batch = api('GET', f'/repos/{repository}/pulls/{number}/files?per_page=100&page={page}')
        result.extend(batch)
        if len(batch) < 100:
            return result
        page += 1


def list_all(path):
    page = 1
    while True:
        separator = '&' if '?' in path else '?'
        batch = api('GET', f'{path}{separator}per_page=100&page={page}')
        yield from batch
        if len(batch) < 100:
            return
        page += 1


def changed_release_controls(files):
    return [file['filename'] for file in files
            if file['filename'] in PROTECTED_PATHS]


def validate_sync_metadata(pr, baseline):
    head_sha = pr['head']['sha']
    if pr['user']['login'] not in ('Copilot', 'copilot-swe-agent[bot]'):
        raise RuntimeError('Sync pull request was not authored by Copilot')
    marker = marker_from(pr.get('body'))
    if not marker:
        raise RuntimeError('Sync pull request is missing its release marker')
    kind, upstream_tag, upstream_sha = marker
    issue = next((item for item in list_all(
                        f'/repos/{repository}/issues?state=all')
                  if f'<!-- upstream-release-sync:{kind}:{upstream_tag}:{upstream_sha} -->'
                  in (item.get('body') or '') and 'pull_request' not in item), None)
    if not issue or not any(user['login'] in ('Copilot', 'copilot-swe-agent[bot]')
                            for user in issue.get('assignees', [])):
        raise RuntimeError('Sync PR does not belong to a Copilot-assigned release issue')
    if not RELEASE_VERSION.fullmatch(upstream_tag):
        raise RuntimeError('Invalid upstream release tag')
    if resolve_upstream_tag(upstream_tag) != upstream_sha:
        raise RuntimeError('Upstream release tag no longer resolves to the pinned commit')
    protected = changed_release_controls(pr_files(pr['number']))
    if protected:
        raise RuntimeError(f'Unexpected change to release control: {protected[0]}')
    if Path('VIM_REVISION').read_text().strip() != str(baseline['vim_revision']):
        raise RuntimeError('VIM_REVISION differs from the approved baseline')
    fetch_candidate(pr, check_base=pr['state'] == 'open')
    candidate_revision = subprocess.check_output(
        ['git', 'show', f'{head_sha}:VIM_REVISION'], text=True).strip()
    if candidate_revision != str(baseline['vim_revision']):
        raise RuntimeError('Sync candidate changed the approved VIM_REVISION')
    candidate_baseline = json.loads(subprocess.check_output(
        ['git', 'show', f'{head_sha}:.github/approved-vim-baseline.json'], text=True))
    if candidate_baseline != baseline:
        raise RuntimeError('Sync candidate changed the approved Vim baseline')
    return kind, upstream_tag


def verify_sync_pr(pr, baseline):
    kind, upstream_tag = validate_sync_metadata(pr, baseline)
    for _ in range(6):
        fresh = api('GET', f'/repos/{repository}/pulls/{pr["number"]}')
        if fresh.get('mergeable_state') != 'unknown':
            break
        time.sleep(5)
    else:
        raise RuntimeError('GitHub could not determine whether the sync PR is conflict-free')
    if fresh['state'] != 'open' or fresh['head']['sha'] != head_sha or fresh['mergeable'] is not True:
        raise RuntimeError('Sync PR is stale, changed, closed, or has unresolved conflicts')
    return fresh, kind, upstream_tag


def fetch_candidate(pr, check_base=True):
    number, sha = pr['number'], pr['head']['sha']
    subprocess.run(['git', 'fetch', 'origin',
                    f'+refs/pull/{number}/head:refs/remotes/verified-pr/{number}'],
                   check=True, capture_output=True)
    fetched = subprocess.check_output(
        ['git', 'rev-parse', f'refs/remotes/verified-pr/{number}'], text=True).strip()
    if fetched != sha:
        raise RuntimeError('Fetched PR head does not match the tested SHA')
    _, upstream_tag, upstream_sha = marker_from(pr.get('body'))
    if subprocess.run(['git', 'merge-base', '--is-ancestor', upstream_sha, sha],
                      check=False).returncode:
        raise RuntimeError(f'Sync candidate does not contain upstream {upstream_tag} at its pinned commit')
    base_sha = pr['base']['sha']
    if check_base and subprocess.run(
            ['git', 'merge-base', '--is-ancestor', base_sha, sha],
            check=False).returncode:
        raise RuntimeError('Sync branch is stale relative to the current base branch')
    files = [item['filename'] for item in pr_files(number)
             if item['status'] != 'removed']
    if files:
        conflict = subprocess.run(
            ['git', 'grep', '-nE', '^(<<<<<<< |=======$|>>>>>>> )', sha, '--', *files],
            check=False, capture_output=True, text=True)
        if conflict.returncode == 0:
            raise RuntimeError('Sync candidate contains unresolved conflict markers')
        if conflict.returncode != 1:
            raise RuntimeError('Could not inspect sync candidate for conflict markers')
    if subprocess.run(['git', 'diff', '--check', f'{base_sha}...{sha}'],
                      check=False, capture_output=True).returncode:
        raise RuntimeError('Sync candidate has whitespace errors')


def sync_pr_for_commit(sha):
    pulls = api('GET', f'/repos/{repository}/commits/{sha}/pulls')
    return next((pr for pr in pulls if pr.get('merge_commit_sha') == sha
                 and marker_from(pr.get('body'))), None)


def dispatch(workflow, ref, inputs=None):
    payload = {'ref': ref}
    if inputs:
        payload['inputs'] = inputs
    api('POST', f'/repos/{repository}/actions/workflows/{workflow}/dispatches', payload)


def promote_candidate(run):
    sha, branch = run['head_sha'], run['head_branch']
    if run['head_repository']['full_name'].lower() != repository.lower():
        raise RuntimeError('Refusing a sync run from another repository')
    baseline = json.loads(Path('.github/approved-vim-baseline.json').read_text())
    if not branch.startswith('copilot/'):
        return
    pair = workflows_pass(sha, {'push'})
    if not pair:
        print(f'Checks for {sha} are missing, stale, pending, skipped, or unsuccessful')
        return
    candidates = api('GET', f'/repos/{repository}/commits/{sha}/pulls')
    pr = next((item for item in candidates
               if item['state'] == 'open' and item['base']['ref'] == baseline['branch']
               and item['head']['sha'] == sha and marker_from(item.get('body'))), None)
    if not pr:
        print(f'No open Copilot sync pull request found for {sha}')
        return
    fresh, _, _ = verify_sync_pr(pr, baseline)
    if branch_sha(baseline['branch']) != fresh['base']['sha']:
        raise RuntimeError('Default branch changed after checks completed')
    merged = api('PUT', f'/repos/{repository}/pulls/{fresh["number"]}/merge', {
        'sha': sha,
        'merge_method': 'merge',
    })
    if not merged.get('merged'):
        raise RuntimeError('GitHub did not merge the verified sync PR')
    merge_sha = merged['sha']
    if branch_sha(baseline['branch']) != merge_sha:
        raise RuntimeError('Default branch advanced unexpectedly during the merge')
    dispatch('cd.yml', baseline['branch'])
    dispatch('vim-platforms.yml', baseline['branch'], {'platform': 'all'})
    print(f'Merged tested PR #{fresh["number"]} at {merge_sha}; dispatched post-merge checks')


def create_release_tag(pr, merge_sha, baseline):
    kind, upstream_tag, upstream_sha = marker_from(pr.get('body'))
    tag = f'texstudio-vim-{upstream_tag}-r{baseline["vim_revision"]}'
    if not RELEASE_VERSION.fullmatch(upstream_tag):
        raise RuntimeError('Invalid upstream release in merged PR marker')
    if branch_sha(baseline['branch']) != merge_sha:
        raise RuntimeError('Default branch changed after post-merge verification')
    if resolve_upstream_tag(upstream_tag) != upstream_sha:
        raise RuntimeError('Upstream release tag moved after the sync was prepared')
    if subprocess.run(['git', 'merge-base', '--is-ancestor', upstream_sha, merge_sha],
                      check=False).returncode:
        raise RuntimeError('Merged release commit does not contain the pinned upstream commit')
    source_tree = api('GET', f'/repos/{repository}/git/commits/{pr["head"]["sha"]}')['tree']['sha']
    merge_tree = api('GET', f'/repos/{repository}/git/commits/{merge_sha}')['tree']['sha']
    if source_tree != merge_tree:
        raise RuntimeError('Merged tree differs from the fully tested PR tree')
    ref_path = f'/repos/{repository}/git/ref/tags/{quote(tag, safe="")}'
    try:
        existing = api('GET', ref_path)['object']
    except Exception as error:
        if getattr(error, 'code', None) != 404:
            raise
        existing = None
    if existing:
        if existing['type'] == 'tag':
            existing = api('GET', f'/repos/{repository}/git/tags/{existing["sha"]}')['object']
        if existing.get('sha') != merge_sha:
            raise RuntimeError(f'Refusing to move existing release tag {tag}')
        print(f'{tag} already points to the verified merge commit')
        try:
            release = api('GET', f'/repos/{repository}/releases/tags/{quote(tag, safe="")}')
        except Exception as error:
            if getattr(error, 'code', None) != 404:
                raise
            release = None
        if release and not release['draft']:
            print(f'{tag} is already published; not rerunning its release workflow')
            return
    else:
        tag_object = api('POST', f'/repos/{repository}/git/tags', {
            'tag': tag,
            'message': f'Texstudio Vim release {tag}',
            'object': merge_sha,
            'type': 'commit',
        })
        api('POST', f'/repos/{repository}/git/refs', {
            'ref': 'refs/tags/' + tag,
            'sha': tag_object['sha'],
        })
        print(f'Created immutable tag {tag} at {merge_sha}')
    dispatch('cd.yml', tag)


def promote_merged_commit(run, baseline):
    sha = run['head_sha']
    pr = sync_pr_for_commit(sha)
    if not pr or not pr.get('merged'):
        return
    pair = workflows_pass(sha, {'workflow_dispatch'})
    if not pair:
        print(f'Post-merge checks for {sha} are missing, stale, skipped, or unsuccessful')
        return
    if pr['head']['repo']['full_name'].lower() != repository.lower():
        raise RuntimeError('Merged sync PR is no longer from this repository')
    marker = marker_from(pr.get('body'))
    if not marker or resolve_upstream_tag(marker[1]) != marker[2]:
        raise RuntimeError('Merged PR release marker is missing or no longer matches upstream')
    if pr['user']['login'] not in ('Copilot', 'copilot-swe-agent[bot]'):
        raise RuntimeError('Merged sync pull request was not authored by Copilot')
    validate_sync_metadata(pr, baseline)
    if subprocess.run(['git', 'merge-base', '--is-ancestor', marker[2], sha],
                      check=False).returncode:
        raise RuntimeError('Merged release commit does not contain its pinned upstream commit')
    create_release_tag(pr, sha, baseline)


def publish_progress(run, baseline):
    tag = run['head_branch']
    match = FORK_RELEASE_TAG.fullmatch(tag)
    if not match or match.group(2) != str(baseline['vim_revision']):
        return
    upstream_tag = match.group(1)
    kind = 'prerelease' if re.search(r'(?:alpha|beta|rc)[0-9]+$', upstream_tag) else 'stable'
    pr = sync_pr_for_commit(run['head_sha'])
    if not pr or not pr.get('merged') or pr['user']['login'] not in (
            'Copilot', 'copilot-swe-agent[bot]'):
        raise RuntimeError('Published tag is not associated with a merged Copilot sync PR')
    marker = marker_from(pr.get('body'))
    if not marker or marker[:2] != (kind, upstream_tag):
        raise RuntimeError('Published tag does not match its sync PR release marker')
    validate_sync_metadata(pr, baseline)
    tag_object = api('GET', f'/repos/{repository}/git/ref/tags/{quote(tag, safe="")}')['object']
    while tag_object['type'] == 'tag':
        tag_object = api('GET', f'/repos/{repository}/git/tags/{tag_object["sha"]}')['object']
    if run['head_sha'] != tag_object['sha']:
        raise RuntimeError('Published release workflow did not use the immutable tag commit')
    release = api('GET', f'/repos/{repository}/releases/tags/{quote(tag, safe="")}')
    if release['draft'] or release['prerelease'] != (kind == 'prerelease'):
        raise RuntimeError('Release is not published with the expected stable/prerelease status')
    upstream = api('GET', f'/repos/{UPSTREAM}/releases/tags/{quote(upstream_tag, safe="")}')
    if upstream['draft'] or upstream['prerelease'] != (kind == 'prerelease'):
        raise RuntimeError('Upstream release type no longer matches the fork tag')
    # Tags may be annotated, so resolve the upstream ref through GitHub's Git API.
    upstream_obj = api('GET', f'/repos/{UPSTREAM}/git/ref/tags/{quote(upstream_tag, safe="")}')['object']
    while upstream_obj['type'] == 'tag':
        upstream_obj = api('GET', f'/repos/{UPSTREAM}/git/tags/{upstream_obj["sha"]}')['object']
    state_sha = upstream_obj['sha']
    if marker[2] != state_sha:
        raise RuntimeError('Fork release marker no longer matches the upstream release commit')
    state_path = '.github/upstream-release-state.json'
    state = api('GET', f'/repos/{repository}/contents/{state_path}?ref={baseline["branch"]}')
    content = json.loads(base64.b64decode(state['content']))
    progress = content[kind]
    if progress.get('upstream_tag') == upstream_tag and progress.get('upstream_sha') == state_sha:
        return
    content[kind] = {
        'upstream_tag': upstream_tag,
        'upstream_sha': state_sha,
        'published_at': upstream['published_at'],
        'fork_tag': tag,
        'fork_sha': run['head_sha'],
    }
    api('PUT', f'/repos/{repository}/contents/{state_path}', {
        'message': f'Record published {kind} upstream sync {upstream_tag}',
        'content': base64.b64encode((json.dumps(content, indent=2) + '\n').encode()).decode(),
        'sha': state['sha'],
        'branch': baseline['branch'],
    })
    print(f'Recorded {kind} progress through upstream {upstream_tag}')


def main():
    run = json.loads(os.environ['WORKFLOW_RUN'])
    if run['head_repository']['full_name'].lower() != repository.lower():
        return
    action = os.environ['CONTROLLER_ACTION']
    baseline = json.loads(Path('.github/approved-vim-baseline.json').read_text())
    if action == 'merge':
        if run['conclusion'] == 'success':
            promote_candidate(run)
        return
    if action == 'tag':
        if run['conclusion'] == 'success' and run['head_branch'] == baseline['branch']:
            promote_merged_commit(run, baseline)
        return
    if action == 'publish' and run['name'] == 'CD' and run['head_branch'].startswith('texstudio-vim-'):
        if require_job_set(run, CD_JOBS | {'Release'}):
            publish_progress(run, baseline)
        return


if __name__ == '__main__':
    main()
