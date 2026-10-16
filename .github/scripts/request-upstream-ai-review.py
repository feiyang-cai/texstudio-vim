#!/usr/bin/env python3
"""Request a separate Copilot review using trusted default-branch code only."""
import os
import json

from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

spec = spec_from_file_location('controller', Path(__file__).with_name('upstream-release-controller.py'))
controller = module_from_spec(spec)
spec.loader.exec_module(controller)
# GitHub can represent an in-flight code-review request using the Copilot alias.
# This alias must not broaden the identities allowed to approve promotion.
PENDING_REVIEWER_LOGINS = controller.AI_REVIEWER_LOGINS | {'Copilot'}


def request_review(pr):
    if (pr['state'] != 'open' or pr.get('draft')
            or pr['user']['login'] not in ('Copilot', 'copilot-swe-agent[bot]')
            or (pr['head'].get('repo') or {}).get('full_name') != controller.repository
            or not controller.marker_from(pr.get('body'))):
        return
    snapshot = controller.review_base_snapshot(pr)
    review = controller.latest_ai_review(pr)
    if (snapshot and snapshot['description'] == pr['base']['sha']
            and review and review['submitted_at'] > snapshot['created_at']):
        print(f'PR #{pr["number"]} already has an AI review for this commit')
        return
    if any(user['login'] in PENDING_REVIEWER_LOGINS for user in pr.get('requested_reviewers', [])):
        print(f'PR #{pr["number"]} is awaiting its independent AI review')
        return
    # Record before requesting so a fast review cannot precede its snapshot.
    # An in-flight review must finish before its snapshot may be replaced.
    controller.api('POST', f'/repos/{controller.repository}/statuses/{pr["head"]["sha"]}', {
        'state': 'success', 'context': controller.AI_BASE_CONTEXT,
        'description': pr['base']['sha'],
        'target_url': f'https://github.com/{controller.repository}/actions/runs/{os.environ["GITHUB_RUN_ID"]}',
    }, token_value=os.environ['REVIEW_STATE_TOKEN'])
    controller.api('POST', f'/repos/{controller.repository}/pulls/{pr["number"]}/requested_reviewers',
                   {'reviewers': [controller.AI_REVIEWER]})
    print(f'Requested independent Copilot review of PR #{pr["number"]} at {pr["head"]["sha"]}')


def main():
    policy = json.loads(Path('.github/upstream-release-policy.json').read_text())
    if policy['dry_run']:
        print('Dry run: independent AI review requests are disabled')
        return
    if not controller.token:
        raise SystemExit('COPILOT_SYNC_TOKEN is required to request Copilot reviews')
    number = os.environ.get('PR_NUMBER')
    numbers = [int(number)] if number else [pr['number'] for pr in controller.list_all(
        f'/repos/{controller.repository}/pulls?state=open')]
    for number in numbers:
        request_review(controller.api('GET', f'/repos/{controller.repository}/pulls/{number}'))


if __name__ == '__main__':
    main()
