#!/usr/bin/env python3
"""Create or reuse Copilot sync issues for newly published upstream releases."""
import json
import os
from pathlib import Path
import re
import subprocess
from urllib.parse import quote
from urllib.request import Request, urlopen

UPSTREAM = 'texstudio-org/texstudio'
RELEASE_VERSION = re.compile(r'[0-9]+\.[0-9]+\.[0-9]+(?:(?:alpha|beta|rc)[0-9]+)?')
API = 'https://api.github.com'


def classify_release(release):
    if release.get('draft') or not RELEASE_VERSION.fullmatch(release.get('tag_name', '')):
        return None
    return 'prerelease' if release.get('prerelease') else 'stable'


def next_release(releases, kind, progress):
    newer = [
        release for release in releases
        if classify_release(release) == kind
        and release.get('published_at')
        and release['tag_name'] != progress['upstream_tag']
        and release['published_at'] >= progress['published_at']
    ]
    return min(newer, key=lambda item: item['published_at']) if newer else None


def api(method, path, payload=None):
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {
        'Accept': 'application/vnd.github+json',
        'X-GitHub-Api-Version': '2022-11-28',
        'Content-Type': 'application/json',
    }
    request_token = os.environ.get('COPILOT_SYNC_TOKEN') if method != 'GET' else None
    if method != 'GET' and not request_token:
        raise SystemExit('COPILOT_SYNC_TOKEN is required to create or assign sync issues')
    if request_token:
        headers['Authorization'] = 'Bearer ' + request_token
    request = Request(API + path, data=data, method=method, headers=headers)
    with urlopen(request, timeout=30) as response:
        return json.load(response) if response.status != 204 else None


def resolve_tag(repository, tag):
    ref = api('GET', f'/repos/{repository}/git/ref/tags/{quote(tag, safe="")}')
    obj = ref['object']
    while obj['type'] == 'tag':
        obj = api('GET', f'/repos/{repository}/git/tags/{obj["sha"]}')['object']
    if obj['type'] != 'commit' or not re.fullmatch(r'[0-9a-f]{40}', obj['sha']):
        raise RuntimeError(f'Upstream tag {tag} does not resolve to a commit')
    return obj['sha']


def list_all(path):
    page = 1
    while True:
        separator = '&' if '?' in path else '?'
        values = api('GET', f'{path}{separator}per_page=100&page={page}')
        yield from values
        if len(values) < 100:
            return
        page += 1


def already_in_history(commit, tag):
    ref = f'refs/remotes/upstream-release/{commit}'
    subprocess.run(['git', 'fetch', '--no-tags',
                    f'https://github.com/{UPSTREAM}.git',
                    f'refs/tags/{tag}:{ref}'], check=True, capture_output=True)
    return subprocess.run(['git', 'merge-base', '--is-ancestor', commit, 'HEAD'],
                          check=False, capture_output=True).returncode == 0


def sync_marker(kind, tag, commit):
    return f'<!-- upstream-release-sync:{kind}:{tag}:{commit} -->'


def sync_instructions(kind, release, commit, baseline):
    marker = sync_marker(kind, release['tag_name'], commit)
    return f"""Automatically prepare the TeXstudio upstream release sync below.

{marker}

- Release type: {kind}
- Exact upstream repository: {UPSTREAM}
- Exact upstream release tag: `{release['tag_name']}`
- Resolved upstream commit (do not substitute a moving branch): `{commit}`
- Approved fork baseline: `{baseline['branch']}` at `{baseline['commit']}`
- Approved `VIM_REVISION`: `{baseline['vim_revision']}`; preserve it unchanged.

Create a separate sync branch from the current tip of `{baseline['branch']}` and
merge the exact upstream tag. The pinned approved fork commit identifies the Vim
implementation to preserve; do not reset the branch to that older commit.
Resolve conflicts while retaining this fork's Vim
support, registers, all modes, motions, search, substitution, undo/redo and
Ctrl-click navigation. Preserve the fresh-config Vim default, saved preferences,
updater, About branding, warning, and feedback links. Do not reset to upstream,
replace editor files wholesale, change release-control files or `VIM_REVISION`,
disable tests, force-push, or move existing release tags. Report any unresolved
conflict or failed test in the pull request.

Open a pull request against `{baseline['branch']}`. Its description must include
this exact marker so the trusted release controller can bind all checks and the
eventual fork tag to this release. Do not include unrelated Vim development.

Run the existing `CD` and `Vim desktop tests` workflows on the sync branch. The
controller will only merge after Linux tests, all native platforms, every
package build and all nine packaged GUI environments pass on the exact PR head.
"""


def main():
    repository = os.environ['GITHUB_REPOSITORY']
    state_path = Path('.github/upstream-release-state.json')
    state = json.loads(state_path.read_text())
    baseline = json.loads(Path('.github/approved-vim-baseline.json').read_text())
    if baseline['branch'] != os.environ.get('DEFAULT_BRANCH', 'master'):
        raise SystemExit('Approved baseline branch is not the repository default branch')
    revision = Path('VIM_REVISION').read_text().strip()
    if revision != str(baseline['vim_revision']):
        raise SystemExit('VIM_REVISION differs from the owner-approved baseline')

    releases = list(list_all(f'/repos/{UPSTREAM}/releases'))
    fork_tags = {item['name'] for item in list_all(f'/repos/{repository}/tags')}
    issues = list(list_all(f'/repos/{repository}/issues?state=all'))
    pulls = list(list_all(f'/repos/{repository}/pulls?state=all'))
    # Issues and PRs are both returned by the issues endpoint; markers make a
    # retry idempotent even if Copilot has already opened a pull request.
    tracked = issues + pulls

    for kind in ('stable', 'prerelease'):
        progress = state[kind]
        release = next_release(releases, kind, progress)
        if not release:
            continue
        tag = release['tag_name']
        fork_tag = f"texstudio-vim-{tag}-r{baseline['vim_revision']}"
        if fork_tag in fork_tags:
            print(f'{fork_tag} already exists; waiting for its release controller')
            continue
        commit = resolve_tag(UPSTREAM, tag)
        if already_in_history(commit, tag):
            print(f'{tag} ({commit}) is already in the fork history; no sync needed')
            continue
        marker_prefix = f'<!-- upstream-release-sync:{kind}:{tag}:'
        matching = [item for item in tracked if marker_prefix in (item.get('body') or '')]
        if matching and sync_marker(kind, tag, commit) not in (matching[0].get('body') or ''):
            raise RuntimeError(f'Pending sync for {tag} pins a different upstream commit')
        if any(item.get('merged') for item in matching if 'pull_request' in item):
            print(f'A sync PR for {tag} is merged; waiting for release verification')
            continue
        if any(item.get('state') == 'open' for item in matching if 'pull_request' in item):
            print(f'A sync PR for {tag} is already open')
            continue
        existing = next((item for item in matching if 'pull_request' not in item), None)
        reopened = False
        if existing and existing['state'] == 'closed':
            api('PATCH', f'/repos/{repository}/issues/{existing["number"]}',
                {'state': 'open'})
            existing['state'] = 'open'
            reopened = True
        if existing and existing['state'] == 'open':
            issue_number = existing['number']
            if reopened or not any(user['login'] in ('Copilot', 'copilot-swe-agent[bot]')
                                   for user in existing.get('assignees', [])):
                api('POST', f'/repos/{repository}/issues/{issue_number}/assignees', {
                    'assignees': ['copilot-swe-agent[bot]'],
                    'agent_assignment': {
                        'target_repo': repository,
                        'base_branch': baseline['branch'],
                        'custom_instructions': 'Follow the upstream release sync instructions in the issue exactly.',
                        'custom_agent': '',
                        'model': '',
                    },
                })
            print(f'Reused sync issue #{issue_number} for {tag}')
            continue

        api('POST', f'/repos/{repository}/issues', {
            'title': f'[Upstream sync] TeXstudio {tag}',
            'body': sync_instructions(kind, release, commit, baseline),
            'assignees': ['copilot-swe-agent[bot]'],
            'agent_assignment': {
                'target_repo': repository,
                'base_branch': baseline['branch'],
                'custom_instructions': 'Follow the upstream release sync instructions in the issue exactly.',
                'custom_agent': '',
                'model': '',
            },
        })
        print(f'Created and assigned sync issue for {tag} ({commit})')


if __name__ == '__main__':
    main()
