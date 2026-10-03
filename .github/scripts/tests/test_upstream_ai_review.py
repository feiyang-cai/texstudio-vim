import importlib.util
import json
import os
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
                patch.object(controller, 'api') as api:
            requester.request_review(self.pr)
            self.assertEqual(api.call_args.args[0], 'POST')
            self.assertEqual(api.call_args.args[2], {'reviewers': [controller.AI_REVIEWER]})
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
                    patch.object(controller, 'api', return_value={'parents': [{'sha': 'a' * 40}]}), \
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

    def test_dry_run_never_requests_review_or_needs_a_secret(self):
        with patch.object(controller, 'token', ''), patch.object(controller, 'api') as api:
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
