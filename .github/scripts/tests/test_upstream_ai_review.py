import importlib.util
import json
import os
import subprocess
import tempfile
from contextlib import chdir
from pathlib import Path
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    'review_request', ROOT / '.github/scripts/request-upstream-ai-review.py')
requester = importlib.util.module_from_spec(spec)
spec.loader.exec_module(requester)
controller = requester.controller


class UpstreamAIReviewTests(unittest.TestCase):
    def setUp(self):
        self.classify_upstream_changes = controller.upstream_changes_need_human
        classification_patch = patch.object(controller, 'upstream_changes_need_human', return_value=False)
        classification_patch.start()
        self.addCleanup(classification_patch.stop)
        self.snapshot = {'description': 'b' * 40, 'created_at': '2026-10-01T00:00:00Z'}
        self.snapshot_patch = patch.object(controller, 'review_base_snapshot', return_value=self.snapshot)
        self.snapshot_patch.start()
        self.addCleanup(self.snapshot_patch.stop)
        self.pr = {
            'number': 42, 'state': 'open', 'draft': False,
            'head': {'sha': 'a' * 40, 'ref': 'copilot/sync',
                     'repo': {'full_name': controller.repository}},
            'base': {'sha': 'b' * 40, 'ref': 'master'},
            'user': {'login': 'copilot-swe-agent[bot]'},
            'body': '<!-- upstream-release-sync:stable:5.0.0:' + 'c' * 40 + ':' + 'd' * 40 + ' -->',
        }

    def review(self, state='APPROVED', **changes):
        result = {'id': 1, 'commit_id': 'a' * 40, 'state': state,
                  'submitted_at': '2026-10-01T00:01:00Z',
                  'user': {'login': controller.AI_REVIEWER, 'type': 'Bot'}}
        result.update(changes)
        return result

    def test_only_explicit_current_independent_bot_approval_passes(self):
        cases = [[], [self.review('COMMENTED')], [self.review('CHANGES_REQUESTED')],
                 [self.review('DISMISSED')], [self.review(commit_id='b' * 40)],
                 [self.review(user={'login': 'Copilot', 'type': 'Bot'})],
                 [self.review(user={'login': 'copilot-swe-agent[bot]', 'type': 'Bot'})],
                 [self.review(user={'login': 'feiyang-cai', 'type': 'User'})]]
        for reviews in cases:
            with self.subTest(reviews=reviews), patch.object(controller, 'list_all', return_value=reviews), \
                    patch.object(controller, 'unresolved_ai_threads') as threads:
                self.assertFalse(controller.ai_review_passes(self.pr))
                threads.assert_not_called()
        with patch.object(controller, 'list_all', return_value=[self.review()]), \
                patch.object(controller, 'unresolved_ai_threads', return_value=False):
            self.assertTrue(controller.ai_review_passes(self.pr))

    def test_later_dismissal_or_comment_invalidates_old_approval(self):
        for state in ('DISMISSED', 'COMMENTED', 'CHANGES_REQUESTED'):
            with patch.object(controller, 'list_all', return_value=[
                    self.review(state, id=2), self.review(id=1)]):
                self.assertFalse(controller.ai_review_passes(self.pr))

    def test_unresolved_findings_block_even_with_approval(self):
        with patch.object(controller, 'list_all', return_value=[self.review()]), \
                patch.object(controller, 'unresolved_ai_threads', return_value=True):
            self.assertFalse(controller.ai_review_passes(self.pr))

    def test_upstream_change_blocks_ai_approval_without_human_validation(self):
        with patch.object(controller, 'list_all', return_value=[self.review()]), \
                patch.object(controller, 'unresolved_ai_threads', return_value=False), \
                patch.object(controller, 'upstream_changes_need_human', return_value=True):
            self.assertFalse(controller.ai_review_passes(self.pr))
            approved = self.review(id=2, user={'login': 'feiyang-cai', 'type': 'User'})
            with patch.object(controller, 'list_all', return_value=[self.review(), approved]):
                self.assertTrue(controller.ai_review_passes(self.pr))

    def test_human_validation_rejects_wrong_identity_stale_or_dismissed_reviews(self):
        owner = self.review(user={'login': 'feiyang-cai', 'type': 'User'})
        for reviews in ([], [self.review()], [dict(owner, commit_id='f' * 40)],
                        [dict(owner, submitted_at=self.snapshot['created_at'])],
                        [dict(owner, user={'login': 'someone-else', 'type': 'User'})],
                        [dict(owner, user={'login': 'feiyang-cai', 'type': 'Bot'})],
                        [owner, dict(owner, id=2, state='DISMISSED')],
                        [owner, dict(owner, id=2, state='CHANGES_REQUESTED')]):
            with self.subTest(reviews=reviews), patch.object(controller, 'list_all', return_value=reviews):
                self.assertFalse(controller.human_validation_passes(self.pr, self.snapshot))
        with patch.object(controller, 'list_all', return_value=[owner]):
            self.assertTrue(controller.human_validation_passes(self.pr, self.snapshot))

    def test_missing_or_escalated_behavior_declaration_requires_human(self):
        preserved = self.pr['body'] + '\n<!-- upstream-behavior:preserved -->'
        for pr in (self.pr, dict(self.pr, body=preserved + '\n<!-- upstream-behavior:human-validation-required -->'),
                   dict(self.pr, body=preserved, labels=[{'name': 'upstream-human-validation-required'}]),
                   dict(self.pr, body=preserved + '\n<!-- upstream-behavior:preserved -->')):
            self.assertTrue(self.classify_upstream_changes(pr))
        with patch.object(controller.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1)):
            self.assertTrue(self.classify_upstream_changes(dict(self.pr, body=preserved)))
        with patch.object(controller.subprocess, 'run', return_value=subprocess.CompletedProcess([], 128)):
            with self.assertRaises(RuntimeError):
                self.classify_upstream_changes(dict(self.pr, body=preserved))

    def test_base_change_or_review_before_snapshot_blocks_approval(self):
        for snapshot in (None, {'description': 'c' * 40, 'created_at': '2026-10-01T00:00:00Z'},
                         {'description': 'b' * 40, 'created_at': '2026-10-01T00:02:00Z'}):
            with self.subTest(snapshot=snapshot), \
                    patch.object(controller, 'list_all', return_value=[self.review()]), \
                    patch.object(controller, 'review_base_snapshot', return_value=snapshot), \
                    patch.object(controller, 'unresolved_ai_threads') as threads:
                self.assertFalse(controller.ai_review_passes(self.pr))
                threads.assert_not_called()
        # After merging, compare with the merge's first parent, not the moving base branch.
        self.pr['base']['sha'] = 'f' * 40
        with patch.object(controller, 'list_all', return_value=[self.review()]), \
                patch.object(controller, 'unresolved_ai_threads', return_value=False):
            self.assertTrue(controller.ai_review_passes(self.pr, 'b' * 40))

    def test_snapshot_requires_trusted_workflow_provenance(self):
        self.snapshot_patch.stop()
        snapshot = dict(self.snapshot, state='success', context=controller.AI_BASE_CONTEXT,
                        creator={'login': 'github-actions[bot]', 'type': 'Bot'},
                        target_url=f'https://github.com/{controller.repository}/actions/runs/123')
        run = {'event': 'pull_request_target', 'path': '.github/workflows/upstream-ai-review.yml',
               'head_repository': {'full_name': controller.repository}, 'head_branch': 'master'}
        with patch.object(controller, 'list_all', return_value=[snapshot]), \
                patch.object(controller, 'api', return_value=run):
            self.assertEqual(controller.review_base_snapshot(self.pr), snapshot)
        for changed_run in (dict(run, event='push'), dict(run, head_branch='copilot/sync'),
                            dict(run, path='.github/workflows/ci.yml')):
            with patch.object(controller, 'list_all', return_value=[snapshot]), \
                    patch.object(controller, 'api', return_value=changed_run):
                self.assertIsNone(controller.review_base_snapshot(self.pr))
        with patch.object(controller, 'list_all', return_value=[dict(snapshot, creator={'login': 'Copilot'})]):
            self.assertIsNone(controller.review_base_snapshot(self.pr))

    def test_base_change_requests_new_review_even_for_unchanged_head(self):
        self.pr['base']['sha'] = 'f' * 40
        with patch.object(controller, 'list_all', return_value=[self.review()]), \
                patch.dict(os.environ, {'GITHUB_RUN_ID': '123', 'REVIEW_STATE_TOKEN': 'test-token'}), \
                patch.object(controller, 'api') as api:
            requester.request_review(self.pr)
            self.assertEqual(len(api.call_args_list), 2)
            self.assertEqual(api.call_args_list[0].args[2]['description'], 'f' * 40)

    def test_review_before_or_equal_to_snapshot_is_retried_in_safe_order(self):
        for submitted in ('2026-09-30T00:00:00Z', self.snapshot['created_at']):
            with self.subTest(submitted=submitted), \
                    patch.object(controller, 'list_all', return_value=[self.review(submitted_at=submitted)]), \
                    patch.dict(os.environ, {'GITHUB_RUN_ID': '123', 'REVIEW_STATE_TOKEN': 'test-token'}), \
                    patch.object(controller, 'api') as api:
                requester.request_review(self.pr)
                self.assertEqual(len(api.call_args_list), 2)
                self.assertIn('/statuses/', api.call_args_list[0].args[1])
                self.assertIn('/requested_reviewers', api.call_args_list[1].args[1])

    def test_snapshot_failure_does_not_request_review_and_request_failure_can_retry(self):
        with patch.object(controller, 'list_all', return_value=[]), \
                patch.dict(os.environ, {'GITHUB_RUN_ID': '123', 'REVIEW_STATE_TOKEN': 'test-token'}):
            with patch.object(controller, 'api', side_effect=RuntimeError('snapshot unavailable')) as api:
                with self.assertRaises(RuntimeError):
                    requester.request_review(self.pr)
                self.assertEqual(api.call_count, 1)
            with patch.object(controller, 'api', side_effect=[{}, RuntimeError('review unavailable')]):
                with self.assertRaises(RuntimeError):
                    requester.request_review(self.pr)
            with patch.object(controller, 'api') as api:
                requester.request_review(self.pr)
                self.assertEqual(api.call_count, 2)

    def test_reviewed_merge_push_is_atomic_against_concurrent_base_advance(self):
        actual_run = subprocess.run
        for concurrent in (False, True):
            with self.subTest(concurrent=concurrent), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                remote, work = root / 'remote.git', root / 'work'
                actual_run(['git', 'init', '--bare', '-q', str(remote)], check=True, capture_output=True)
                actual_run(['git', 'init', '-q', '-b', 'master', str(work)], check=True)
                def git(*args):
                    return actual_run(['git', '-C', str(work), *args], check=True,
                                      capture_output=True, text=True).stdout.strip()
                git('config', 'user.name', 'Test')
                git('config', 'user.email', 'test@example.com')
                git('remote', 'add', 'origin', str(remote))
                (work / 'document').write_text('base\n')
                git('add', 'document')
                git('commit', '-qm', 'base')
                base = git('rev-parse', 'HEAD')
                git('push', '-q', 'origin', 'master')
                git('checkout', '-qb', 'copilot/sync')
                (work / 'document').write_text('reviewed upstream change\n')
                git('commit', '-qam', 'reviewed change')
                head = git('rev-parse', 'HEAD')
                git('push', '-q', 'origin', 'copilot/sync')
                git('checkout', '-q', 'master')
                pr = dict(self.pr, base={'sha': base, 'ref': 'master'}, head={'sha': head})
                advanced = []
                def run_with_race(command, **kwargs):
                    if concurrent and command[:3] == ['git', 'push', 'origin']:
                        git('commit', '--allow-empty', '-qm', 'concurrent default-branch change')
                        advanced.append(git('rev-parse', 'HEAD'))
                        git('push', '-q', 'origin', 'master')
                    return actual_run(command, **kwargs)
                with chdir(work), patch.object(controller.subprocess, 'run', side_effect=run_with_race):
                    if concurrent:
                        with self.assertRaises(subprocess.CalledProcessError):
                            controller.merge_reviewed_commits(pr)
                    else:
                        merge = controller.merge_reviewed_commits(pr)
                remote_tip = actual_run(['git', '--git-dir', str(remote), 'rev-parse', 'master'],
                                        check=True, capture_output=True, text=True).stdout.strip()
                if concurrent:
                    self.assertEqual(remote_tip, advanced[0])
                else:
                    self.assertEqual(remote_tip, merge)
                    self.assertEqual(git('rev-list', '--parents', '-n', '1', merge).split(),
                                     [merge, base, head])
                    self.assertEqual(git('show', f'{merge}:document'), 'reviewed upstream change')
                    candidate = dict(pr, head={'sha': merge}, body=(
                        f'<!-- upstream-release-sync:stable:5.0.0:{head}:{base} -->\n'
                        '<!-- upstream-behavior:preserved -->'))
                    with chdir(work):
                        self.assertFalse(self.classify_upstream_changes(candidate))
                    # An agent can claim preservation while modifying the merged
                    # tree. The controller still detects those extra edits.
                    (work / 'document').write_text('extra upstream edit\n')
                    git('add', 'document')
                    edited_tree = git('write-tree')
                    edited = git('commit-tree', edited_tree, '-p', base, '-p', head, '-m', 'proposal')
                    candidate['head']['sha'] = edited
                    with chdir(work):
                        self.assertTrue(self.classify_upstream_changes(candidate))

    def test_thread_pagination_and_graphql_bot_login(self):
        def page(nodes, more=False):
            return {'data': {'repository': {'pullRequest': {'reviewThreads': {
                'nodes': nodes, 'pageInfo': {'hasNextPage': more, 'endCursor': 'next'},
            }}}}}
        def thread(resolved, login):
            return {'isResolved': resolved, 'comments': {'nodes': [{'author': {'login': login}}]}}
        with patch.object(controller, 'api', side_effect=[
                page([thread(True, controller.AI_REVIEWER), thread(False, 'human')], True),
                page([thread(False, controller.AI_REVIEWER.removesuffix('[bot]'))])]) as api:
            self.assertTrue(controller.unresolved_ai_threads(42))
            self.assertEqual(api.call_args_list[1].args[2]['variables']['cursor'], 'next')
        with patch.object(controller, 'api', return_value={'errors': [{'message': 'denied'}]}):
            with self.assertRaises(RuntimeError):
                controller.unresolved_ai_threads(42)

    def test_missing_ai_approval_prevents_merge_even_when_checks_pass(self):
        run = {'head_sha': 'a' * 40, 'head_branch': 'copilot/sync',
               'head_repository': {'full_name': controller.repository}}
        with patch.object(controller, 'workflows_pass', return_value=True), \
                patch.object(controller, 'api', return_value=[self.pr]) as api, \
                patch.object(controller, 'verify_sync_pr', return_value=(self.pr, 'stable', '5.0.0')), \
                patch.object(controller, 'ai_review_passes', return_value=False), \
                patch.object(controller, 'create_release_candidate') as release:
            controller.promote_candidate(run, allow_publication=True)
            self.assertEqual([call.args[0] for call in api.call_args_list], ['GET'])
            release.assert_not_called()

    def test_requests_review_again_after_new_commit_but_not_while_pending(self):
        with patch.object(controller, 'list_all', return_value=[self.review(commit_id='b' * 40)]), \
                patch.dict(os.environ, {'GITHUB_RUN_ID': '123', 'REVIEW_STATE_TOKEN': 'test-token'}), \
                patch.object(controller, 'api') as api:
            requester.request_review(self.pr)
            self.assertEqual(api.call_args_list[0].args[0], 'POST')
            self.assertEqual(api.call_args.args[2], {'reviewers': [controller.AI_REVIEWER]})
            self.assertEqual(api.call_args_list[0].args[2]['description'], self.pr['base']['sha'])
        for login in (controller.AI_REVIEWER, controller.AI_REVIEWER.removesuffix('[bot]'), 'Copilot'):
            self.pr['requested_reviewers'] = [{'login': login}]
            with self.subTest(login=login), \
                    patch.object(controller, 'list_all', return_value=[]), \
                    patch.object(controller, 'api') as api:
                requester.request_review(self.pr)
                api.assert_not_called()

    def test_missing_or_stale_ai_approval_prevents_release_tagging(self):
        run = {'head_sha': 'e' * 40, 'head_branch': 'release-sync/stable/5.0.0'}
        pr = dict(self.pr, merged=True, merge_commit_sha='f' * 40)
        for reviews in ([], [self.review(commit_id='b' * 40)]):
            with self.subTest(reviews=reviews), \
                    patch.object(controller, 'workflows_pass', return_value=True), \
                    patch.object(controller.subprocess, 'run'), \
                    patch.object(controller.subprocess, 'check_output', return_value=run['head_sha']), \
                    patch.object(controller, 'api', side_effect=[
                        {'parents': [{'sha': 'a' * 40}]},
                        {'parents': [{'sha': 'b' * 40}, {'sha': 'a' * 40}]}]), \
                    patch.object(controller, 'sync_pr_for_commit', return_value=pr), \
                    patch.object(controller, 'resolve_upstream_tag', return_value='c' * 40), \
                    patch.object(controller, 'validate_sync_metadata'), \
                    patch.object(controller, 'list_all', return_value=reviews), \
                    patch.object(controller, 'create_release_tag') as tag:
                controller.promote_release_candidate(run, {'branch': 'master'})
                tag.assert_not_called()

    def test_daily_recheck_preserves_publication_and_candidate_filters(self):
        for dry_run, policy_enabled, variable_enabled, expected in (
                (False, False, 'true', None),
                (False, True, 'false', None),
                (True, False, 'false', (True, False)),
                (False, True, 'true', (False, True))):
            policy = {'dry_run': dry_run, 'publication_enabled': policy_enabled}
            prs = [self.pr, dict(self.pr, draft=True), dict(self.pr, body=''),
                   dict(self.pr, user={'login': 'human'}),
                   dict(self.pr, head={'repo': {'full_name': 'other/repo'}})]
            with self.subTest(policy=policy, variable=variable_enabled), \
                    patch.dict(os.environ, {'CONTROLLER_ACTION': 'pending',
                                            'UPSTREAM_RELEASE_PUBLICATION_ENABLED': variable_enabled}), \
                    patch.object(controller.Path, 'read_text', return_value=json.dumps(policy)), \
                    patch.object(controller, 'list_all', return_value=prs) as listing, \
                    patch.object(controller, 'promote_candidate') as promote:
                controller.main()
                if expected is None:
                    listing.assert_not_called()
                    promote.assert_not_called()
                else:
                    promote.assert_called_once_with(
                        {'head_sha': 'a' * 40, 'head_branch': 'copilot/sync',
                         'head_repository': {'full_name': controller.repository}}, *expected)

    def test_daily_recheck_continues_after_candidate_failure_and_reports_it(self):
        second = dict(self.pr, number=43, head=dict(self.pr['head'], sha='f' * 40))
        policy = {'dry_run': False, 'publication_enabled': True}
        with patch.dict(os.environ, {'CONTROLLER_ACTION': 'pending',
                                    'UPSTREAM_RELEASE_PUBLICATION_ENABLED': 'true'}), \
                patch.object(controller.Path, 'read_text', return_value=json.dumps(policy)), \
                patch.object(controller, 'list_all', return_value=[self.pr, second]), \
                patch.object(controller, 'promote_candidate', side_effect=[RuntimeError('bad candidate'), None]) as promote:
            with self.assertRaisesRegex(RuntimeError, r'PRs: \[42\]'):
                controller.main()
            self.assertEqual(promote.call_count, 2)
            self.assertEqual(promote.call_args.args[0]['head_sha'], 'f' * 40)

    def test_dry_run_never_requests_review_or_needs_a_secret(self):
        with patch.object(controller.Path, 'read_text', return_value=json.dumps({'dry_run': True})), \
                patch.object(controller, 'token', ''), patch.object(controller, 'api') as api:
            requester.main()
            api.assert_not_called()

    def test_active_review_requests_require_user_token_before_any_api_call(self):
        with patch.object(controller.Path, 'read_text', return_value=json.dumps({'dry_run': False})), \
                patch.object(controller, 'token', ''), patch.object(controller, 'api') as api:
            with self.assertRaisesRegex(SystemExit, 'COPILOT_SYNC_TOKEN is required'):
                requester.main()
            api.assert_not_called()

    def test_foreign_human_and_draft_prs_do_not_start_ai_sessions(self):
        for changes in ({'draft': True}, {'state': 'closed'}, {'body': ''},
                        {'user': {'login': 'human'}},
                        {'head': {'repo': {'full_name': 'other/repo'}}}):
            with patch.object(controller, 'api') as api:
                requester.request_review(dict(self.pr, **changes))
                api.assert_not_called()


if __name__ == '__main__':
    unittest.main()
