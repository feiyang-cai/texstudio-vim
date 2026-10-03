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
MARKER = re.compile(r'<!-- upstream-release-sync:(stable|prerelease):([0-9]+\.[0-9]+\.[0-9]+(?:(?:alpha|beta|rc)[0-9]+)?):([0-9a-f]{40}):([0-9a-f]{40}) -->')
RELEASE_VERSION = re.compile(r'[0-9]+\.[0-9]+\.[0-9]+(?:(?:alpha|beta|rc)[0-9]+)?')
PROTECTED_PATHS = {
    'VIM_REVISION',
    '.github/approved-vim-baseline.json',
    '.github/upstream-release-state.json',
    '.github/upstream-release-policy.json',
    '.github/release-request.json',
    '.github/copilot-instructions.md',
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
AI_REVIEWER = 'copilot-pull-request-reviewer[bot]'
AI_REVIEWER_LOGINS = {AI_REVIEWER, AI_REVIEWER.removesuffix('[bot]')}


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
    return tuple(match.groups()) if match else None


def latest_ai_review(pr):
    reviews = list(list_all(f'/repos/{repository}/pulls/{pr["number"]}/reviews'))
    matching = [review for review in reviews
                if review.get('user', {}).get('login') in AI_REVIEWER_LOGINS
                and review.get('user', {}).get('type') == 'Bot'
                and review.get('commit_id') == pr['head']['sha']]
    return max(matching, key=lambda review: review['id']) if matching else None


def unresolved_ai_threads(number):
    query = '''query($owner:String!, $repo:String!, $number:Int!, $cursor:String) {
      repository(owner:$owner, name:$repo) {
        pullRequest(number:$number) {
          reviewThreads(first:100, after:$cursor) {
            nodes { isResolved comments(first:1) { nodes { author { login } } } }
            pageInfo { hasNextPage endCursor }
          }
        }
      }
    }'''
    cursor = None
    while True:
        response = api('POST', '/graphql', {
            'query': query,
            'variables': {'owner': owner, 'repo': repo, 'number': number, 'cursor': cursor},
        })
        if response.get('errors'):
            raise RuntimeError('Could not verify AI review threads')
        threads = response['data']['repository']['pullRequest']['reviewThreads']
        for thread in threads['nodes']:
            comments = thread['comments']['nodes']
            if (not thread['isResolved'] and comments
                    and (comments[0].get('author') or {}).get('login') in AI_REVIEWER_LOGINS):
                return True
        if not threads['pageInfo']['hasNextPage']:
            return False
        cursor = threads['pageInfo']['endCursor']


def ai_review_passes(pr):
    review = latest_ai_review(pr)
    if not review or review['state'] != 'APPROVED':
        print(f'PR #{pr["number"]} needs independent Copilot approval of {pr["head"]["sha"]}')
        return False
    if unresolved_ai_threads(pr['number']):
        print(f'PR #{pr["number"]} still has unresolved Copilot review findings')
        return False
    return True


def publication_enabled(policy):
    return (policy.get('publication_enabled') is True
            and os.environ.get('UPSTREAM_RELEASE_PUBLICATION_ENABLED', '').lower() == 'true')


def candidate_uses_published_base(first_parent, published_base):
    return first_parent == published_base


def candidate_is_pinned_merge(parents, commit, fork_base, upstream_base):
    return parents == [commit, fork_base, upstream_base]


def trusted_overlay_only(paths):
    return bool(paths) and all(path.startswith('.github/') for path in paths)


def has_approved_revision(revision, baseline):
    return revision.strip() == str(baseline['vim_revision'])


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
    protected = []
    for file in files:
        for name in (file.get('filename'), file.get('previous_filename')):
            if name and (name in PROTECTED_PATHS or name.startswith((
                    '.github/scripts/', '.github/workflows/', '.github/actions/'))):
                protected.append(name)
    return protected


def check_pr_head_is_current(pr, fresh):
    if (fresh['state'] != 'open' or fresh['head']['sha'] != pr['head']['sha']
            or fresh['mergeable'] is not True):
        raise RuntimeError('Sync PR is stale, changed, closed, or has unresolved conflicts')


def validate_sync_metadata(pr, baseline):
    head_sha = pr['head']['sha']
    if pr['user']['login'] not in ('Copilot', 'copilot-swe-agent[bot]'):
        raise RuntimeError('Sync pull request was not authored by Copilot')
    marker = marker_from(pr.get('body'))
    if not marker:
        raise RuntimeError('Sync pull request is missing its release marker')
    kind, upstream_tag, upstream_sha, fork_base = marker
    issue = next((item for item in list_all(
                        f'/repos/{repository}/issues?state=all')
                  if f'<!-- upstream-release-sync:{kind}:{upstream_tag}:{upstream_sha}:{fork_base} -->'
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
    if not has_approved_revision(Path('VIM_REVISION').read_text(), baseline):
        raise RuntimeError('VIM_REVISION differs from the approved baseline')
    fetch_candidate(pr)
    candidate_revision = subprocess.check_output(
        ['git', 'show', f'{head_sha}:VIM_REVISION'], text=True).strip()
    if not has_approved_revision(candidate_revision, baseline):
        raise RuntimeError('Sync candidate changed the approved VIM_REVISION')
    state = json.loads(Path('.github/upstream-release-state.json').read_text())
    if state[kind]['fork_sha'] != fork_base:
        raise RuntimeError('Sync candidate is not based on the latest published stream release')
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
    check_pr_head_is_current(pr, fresh)
    return fresh, kind, upstream_tag


def fetch_candidate(pr):
    number, sha = pr['number'], pr['head']['sha']
    subprocess.run(['git', 'fetch', 'origin',
                    f'+refs/pull/{number}/head:refs/remotes/verified-pr/{number}'],
                   check=True, capture_output=True)
    fetched = subprocess.check_output(
        ['git', 'rev-parse', f'refs/remotes/verified-pr/{number}'], text=True).strip()
    if fetched != sha:
        raise RuntimeError('Fetched PR head does not match the tested SHA')
    _, upstream_tag, upstream_sha, fork_base = marker_from(pr.get('body'))
    baseline = json.loads(Path('.github/approved-vim-baseline.json').read_text())
    published_revision = subprocess.check_output(
        ['git', 'show', f'{fork_base}:VIM_REVISION'], text=True).strip()
    if not has_approved_revision(published_revision, baseline):
        raise RuntimeError('Published fork base does not match the approved Vim revision')
    first_parent = subprocess.check_output(
        ['git', 'rev-parse', f'{sha}^1'], text=True).strip()
    if not candidate_uses_published_base(first_parent, fork_base):
        raise RuntimeError('Sync candidate is not based exactly on its published fork baseline')
    parents = subprocess.check_output(
        ['git', 'rev-list', '--parents', '-n', '1', sha], text=True).split()
    if not candidate_is_pinned_merge(parents, sha, fork_base, upstream_sha):
        raise RuntimeError('Sync candidate must be one merge of its published fork and pinned upstream bases')
    if subprocess.run(['git', 'merge-base', '--is-ancestor', upstream_sha, sha],
                      check=False).returncode:
        raise RuntimeError(f'Sync candidate does not contain upstream {upstream_tag} at its pinned commit')
    files = subprocess.check_output(
        ['git', 'diff', '--name-only', f'{fork_base}..{sha}'], text=True).splitlines()
    if files:
        conflict = subprocess.run(
            ['git', 'grep', '-nE', '^(<<<<<<< |=======$|>>>>>>> )', sha, '--', *files],
            check=False, capture_output=True, text=True)
        if conflict.returncode == 0:
            raise RuntimeError('Sync candidate contains unresolved conflict markers')
        if conflict.returncode != 1:
            raise RuntimeError('Could not inspect sync candidate for conflict markers')
    if subprocess.run(['git', 'diff', '--check', f'{fork_base}..{sha}'],
                      check=False, capture_output=True).returncode:
        raise RuntimeError('Sync candidate has whitespace errors')


def sync_pr_for_commit(sha):
    pulls = api('GET', f'/repos/{repository}/commits/{sha}/pulls')
    return next((pr for pr in pulls if (pr.get('merge_commit_sha') == sha
                 or pr['head']['sha'] == sha) and marker_from(pr.get('body'))), None)


def sync_pr_for_release_commit(sha):
    commit = api('GET', f'/repos/{repository}/git/commits/{sha}')
    candidates = [sha] + [parent['sha'] for parent in commit['parents']]
    for candidate in candidates:
        pr = sync_pr_for_commit(candidate)
        if pr and pr.get('merged') and marker_from(pr.get('body')):
            return pr
    return None


def dispatch(workflow, ref, inputs=None):
    payload = {'ref': ref}
    if inputs:
        payload['inputs'] = inputs
    api('POST', f'/repos/{repository}/actions/workflows/{workflow}/dispatches', payload)


def create_release_candidate(pr, baseline):
    candidate_sha = pr['head']['sha']
    _, upstream_tag, _, _ = marker_from(pr.get('body'))
    tag = f'texstudio-vim-{upstream_tag}-r{baseline["vim_revision"]}'
    branch = f'release-sync/{tag}'
    trusted_github_tree = subprocess.check_output(
        ['git', 'rev-parse', 'HEAD:.github'], text=True).strip()
    candidate = api('GET', f'/repos/{repository}/git/commits/{candidate_sha}')
    tree = api('POST', f'/repos/{repository}/git/trees', {
        'base_tree': candidate['tree']['sha'],
        'tree': [{
            'path': '.github',
            'mode': '040000',
            'type': 'tree',
            'sha': trusted_github_tree,
        }],
    })
    ref_path = f'/repos/{repository}/git/ref/heads/{quote(branch, safe="/")}'
    try:
        ref = api('GET', ref_path)
    except Exception as error:
        if getattr(error, 'code', None) != 404:
            raise
        ref = None
    if ref:
        release_sha = ref['object']['sha']
        release_commit = api('GET', f'/repos/{repository}/git/commits/{release_sha}')
        if (release_commit['tree']['sha'] != tree['sha']
                or [parent['sha'] for parent in release_commit['parents']] != [candidate_sha]):
            raise RuntimeError(f'Refusing to reuse unexpected release candidate branch {branch}')
    else:
        release_commit = api('POST', f'/repos/{repository}/git/commits', {
            'message': f'Prepare verified release build {tag}',
            'tree': tree['sha'],
            'parents': [candidate_sha],
        })
        release_sha = release_commit['sha']
        api('POST', f'/repos/{repository}/git/refs', {
            'ref': f'refs/heads/{branch}',
            'sha': release_sha,
        })
    dispatch('cd.yml', branch)
    dispatch('vim-platforms.yml', branch, {'platform': 'all'})
    print(f'Dispatched full verification for {tag} at {release_sha}')


def promote_candidate(run, dry_run=False, allow_publication=False):
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
    if not ai_review_passes(fresh):
        return
    if branch_sha(baseline['branch']) != fresh['base']['sha']:
        raise RuntimeError('Default branch changed after checks completed')
    if dry_run:
        print(f'Dry run: verified PR #{fresh["number"]} at {sha}; would merge and test it')
        return
    if not allow_publication:
        print('Publication disabled: verified sync PR is not merged')
        return
    merged = api('PUT', f'/repos/{repository}/pulls/{fresh["number"]}/merge', {
        'sha': sha,
        'merge_method': 'merge',
    })
    if not merged.get('merged'):
        raise RuntimeError('GitHub did not merge the verified sync PR')
    merge_sha = merged['sha']
    if branch_sha(baseline['branch']) != merge_sha:
        raise RuntimeError('Default branch advanced unexpectedly during the merge')
    create_release_candidate(fresh, baseline)
    print(f'Merged tested PR #{fresh["number"]} at {merge_sha}; dispatched release candidate checks')


def create_release_tag(pr, merge_sha, release_sha, baseline, dry_run=False):
    _, upstream_tag, upstream_sha, _ = marker_from(pr.get('body'))
    tag = f'texstudio-vim-{upstream_tag}-r{baseline["vim_revision"]}'
    if not RELEASE_VERSION.fullmatch(upstream_tag):
        raise RuntimeError('Invalid upstream release in merged PR marker')
    subprocess.run(['git', 'fetch', 'origin',
                    f'+refs/heads/{baseline["branch"]}:refs/remotes/verified-default'],
                   check=True, capture_output=True)
    if subprocess.run(['git', 'merge-base', '--is-ancestor', merge_sha,
                       'refs/remotes/verified-default'], check=False).returncode:
        raise RuntimeError('Merged sync PR is no longer on the default branch')
    if resolve_upstream_tag(upstream_tag) != upstream_sha:
        raise RuntimeError('Upstream release tag moved after the sync was prepared')
    if subprocess.run(['git', 'merge-base', '--is-ancestor', upstream_sha, release_sha],
                      check=False).returncode:
        raise RuntimeError('Published release commit does not contain the pinned upstream commit')
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
        if existing.get('sha') != release_sha:
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
        if dry_run:
            print(f'Dry run: verified merge would create tag {tag} at {release_sha}')
            return
        tag_object = api('POST', f'/repos/{repository}/git/tags', {
            'tag': tag,
            'message': f'Texstudio Vim release {tag}',
            'object': release_sha,
            'type': 'commit',
        })
        api('POST', f'/repos/{repository}/git/refs', {
            'ref': 'refs/tags/' + tag,
            'sha': tag_object['sha'],
        })
        print(f'Created immutable tag {tag} at {release_sha}')
    if dry_run:
        print(f'Dry run: verified merge would dispatch CD for existing tag {tag}')
        return
    dispatch('cd.yml', tag)


def promote_release_candidate(run, baseline, dry_run=False):
    sha = run['head_sha']
    if not run['head_branch'].startswith('release-sync/'):
        return
    pair = workflows_pass(sha, {'workflow_dispatch'})
    if not pair:
        print(f'Release candidate checks for {sha} are missing, stale, skipped, or unsuccessful')
        return
    subprocess.run(['git', 'fetch', 'origin',
                    f'+refs/heads/{run["head_branch"]}:refs/remotes/verified-release-candidate'],
                   check=True, capture_output=True)
    fetched = subprocess.check_output(
        ['git', 'rev-parse', 'refs/remotes/verified-release-candidate'], text=True).strip()
    if fetched != sha:
        raise RuntimeError('Release candidate branch changed after verification')
    commit = api('GET', f'/repos/{repository}/git/commits/{sha}')
    if len(commit['parents']) != 1:
        raise RuntimeError('Release candidate must have exactly one parent')
    candidate_sha = commit['parents'][0]['sha']
    pr = sync_pr_for_commit(candidate_sha)
    if not pr or not pr.get('merged'):
        return
    if (pr['head']['sha'] != candidate_sha
            or pr['head']['repo']['full_name'].lower() != repository.lower()):
        raise RuntimeError('Merged sync PR is no longer from this repository')
    marker = marker_from(pr.get('body'))
    if not marker or resolve_upstream_tag(marker[1]) != marker[2]:
        raise RuntimeError('Merged PR release marker is missing or no longer matches upstream')
    if pr['user']['login'] not in ('Copilot', 'copilot-swe-agent[bot]'):
        raise RuntimeError('Merged sync pull request was not authored by Copilot')
    candidate_checks = workflows_pass(candidate_sha, {'push'})
    if not candidate_checks:
        raise RuntimeError('Exact source candidate checks are missing or failed')
    validate_sync_metadata(pr, baseline)
    if not ai_review_passes(pr):
        return
    changed = subprocess.check_output(
        ['git', 'diff', '--name-only', f'{candidate_sha}..{sha}'], text=True).splitlines()
    if not trusted_overlay_only(changed):
        raise RuntimeError('Release candidate may only overlay trusted controller and test files')
    trusted_github_tree = subprocess.check_output(
        ['git', 'rev-parse', 'HEAD:.github'], text=True).strip()
    candidate_github_tree = subprocess.check_output(
        ['git', 'rev-parse', f'{sha}:.github'], text=True).strip()
    if candidate_github_tree != trusted_github_tree:
        raise RuntimeError('Release candidate does not use the trusted base-branch controller and tests')
    if not pr.get('merge_commit_sha'):
        raise RuntimeError('Merged sync PR has no merge commit')
    subprocess.run(['git', 'fetch', 'origin',
                    f'+refs/heads/{baseline["branch"]}:refs/remotes/verified-default'],
                   check=True, capture_output=True)
    if subprocess.run(['git', 'merge-base', '--is-ancestor', pr['merge_commit_sha'],
                       'refs/remotes/verified-default'], check=False).returncode:
        raise RuntimeError('Merged sync PR is no longer on the default branch')
    create_release_tag(pr, pr['merge_commit_sha'], sha, baseline, dry_run)


def publish_progress(run, baseline):
    tag = run['head_branch']
    match = FORK_RELEASE_TAG.fullmatch(tag)
    if not match or match.group(2) != str(baseline['vim_revision']):
        return
    upstream_tag = match.group(1)
    kind = 'prerelease' if re.search(r'(?:alpha|beta|rc)[0-9]+$', upstream_tag) else 'stable'
    pr = sync_pr_for_release_commit(run['head_sha'])
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
    release_commit = api('GET', f'/repos/{repository}/git/commits/{run["head_sha"]}')
    if len(release_commit['parents']) != 1 or release_commit['parents'][0]['sha'] != pr['head']['sha']:
        raise RuntimeError('Published release does not use its verified source candidate')
    subprocess.run(['git', 'fetch', 'origin', run['head_sha']],
                   check=True, capture_output=True)
    changed = subprocess.check_output(
        ['git', 'diff', '--name-only', f'{pr["head"]["sha"]}..{run["head_sha"]}'],
        text=True).splitlines()
    if not trusted_overlay_only(changed):
        raise RuntimeError('Published release contains changes beyond the trusted controller and test overlay')
    trusted_github_tree = subprocess.check_output(
        ['git', 'rev-parse', 'HEAD:.github'], text=True).strip()
    release_github_tree = subprocess.check_output(
        ['git', 'rev-parse', f'{run["head_sha"]}:.github'], text=True).strip()
    if release_github_tree != trusted_github_tree:
        raise RuntimeError('Published release does not use the trusted base-branch controller and tests')
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
    action = os.environ['CONTROLLER_ACTION']
    if action == 'pending':
        policy = json.loads(Path('.github/upstream-release-policy.json').read_text())
        if not policy['dry_run'] and not publication_enabled(policy):
            print('Publication disabled: pending syncs are not merged')
            return
        for pr in list_all(f'/repos/{repository}/pulls?state=open'):
            if (not pr.get('draft') and marker_from(pr.get('body'))
                    and (pr['head'].get('repo') or {}).get('full_name') == repository
                    and pr['user']['login'] in ('Copilot', 'copilot-swe-agent[bot]')):
                promote_candidate({'head_sha': pr['head']['sha'],
                                   'head_branch': pr['head']['ref'],
                                   'head_repository': {'full_name': repository}},
                                  policy['dry_run'], publication_enabled(policy))
        return
    run = json.loads(os.environ['WORKFLOW_RUN'])
    if run['head_repository']['full_name'].lower() != repository.lower():
        return
    baseline = json.loads(Path('.github/approved-vim-baseline.json').read_text())
    if action == 'merge':
        if run['conclusion'] == 'success':
            policy = json.loads(Path('.github/upstream-release-policy.json').read_text())
            promote_candidate(run, policy['dry_run'], publication_enabled(policy))
        return
    if action == 'tag':
        if run['conclusion'] == 'success' and run['head_branch'].startswith('release-sync/'):
            policy = json.loads(Path('.github/upstream-release-policy.json').read_text())
            if not policy['dry_run'] and not publication_enabled(policy):
                print('Publication disabled: verified sync candidate will not be tagged')
                return
            promote_release_candidate(run, baseline, policy['dry_run'])
        return
    if action == 'publish' and run['name'] == 'CD' and run['head_branch'].startswith('texstudio-vim-'):
        policy = json.loads(Path('.github/upstream-release-policy.json').read_text())
        if policy['dry_run'] or not publication_enabled(policy):
            print('Dry run or publication disabled: would record completed release progress')
            return
        if require_job_set(run, CD_JOBS | {'Release'}):
            publish_progress(run, baseline)
        return


if __name__ == '__main__':
    main()
