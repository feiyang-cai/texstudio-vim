"""Real-Git regressions for reviewed integration and staged release identity."""
import base64
import importlib.util
import json
from contextlib import chdir
from pathlib import Path
import subprocess
import tempfile
import unittest
from urllib.error import HTTPError
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    'release_source_controller', ROOT / '.github/scripts/upstream-release-controller.py')
controller = importlib.util.module_from_spec(spec)
spec.loader.exec_module(controller)


class ReleaseSourceIntegrityTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.work, self.remote = root / 'work', root / 'remote.git'
        subprocess.run(['git', 'init', '--bare', '-q', str(self.remote)], check=True)
        subprocess.run(['git', 'init', '-q', '-b', 'master', str(self.work)], check=True)
        self.git('config', 'user.name', 'Test')
        self.git('config', 'user.email', 'test@example.invalid')
        self.git('config', 'core.autocrlf', 'false')
        self.git('remote', 'add', 'origin', str(self.remote))
        self.baseline = {'branch': 'master', 'vim_revision': '1.1'}
        self.write('.github/approved-vim-baseline.json', json.dumps(self.baseline) + '\n')
        self.write('.github/controller', 'published controller\n')
        self.write('VIM_REVISION', '1.1\n')
        self.write('application', 'published fork\n')
        self.fork = self.commit('published fork')
        self.git('checkout', '-qb', 'upstream')
        self.write('upstream-feature', 'new upstream release\n')
        self.upstream = self.commit('upstream release')
        self.git('checkout', '-qb', 'copilot/sync', self.fork)
        self.git('merge', '--no-ff', '-qm', 'pinned upstream sync', self.upstream)
        self.candidate = self.git('rev-parse', 'HEAD')
        self.git('checkout', '-q', 'master')
        self.write('master-only-feature', 'previously reviewed stable AppImage updater\n')
        self.write('.github/controller', 'current trusted controller\n')
        self.base = self.commit('reviewed base with stable feature and trusted controls')
        tree = self.git('merge-tree', '--write-tree', self.base, self.candidate)
        self.merge = self.commit_tree(tree, [self.base, self.candidate])
        self.git('push', '-q', 'origin', f'{self.merge}:refs/heads/master',
                 f'{self.candidate}:refs/pull/42/head')
        self.pr = {
            'number': 42, 'merged': True, 'merge_commit_sha': self.merge,
            'base': {'sha': self.base, 'ref': 'master'},
            'head': {'sha': self.candidate, 'ref': 'copilot/sync',
                     'repo': {'full_name': controller.repository}},
            'user': {'login': 'copilot-swe-agent[bot]'},
            'body': f'<!-- upstream-release-sync:stable:5.0.0:{self.upstream}:{self.fork} -->',
        }
        self.branch = 'release-sync/texstudio-vim-5.0.0-r1.1'
        self.calls = []
        self.release = None
        cwd = chdir(self.work)
        cwd.__enter__()
        self.addCleanup(cwd.__exit__, None, None, None)

    def git(self, *args, input=None):
        return subprocess.run(['git', '-C', str(self.work), *args], input=input,
                              capture_output=True, text=True, check=True).stdout.strip()

    def write(self, name, content):
        path = self.work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def commit(self, message):
        self.git('add', '--all')
        self.git('commit', '-qm', message)
        return self.git('rev-parse', 'HEAD')

    def commit_tree(self, tree, parents):
        arguments = ['commit-tree', tree, '-m', 'fixture integration or release']
        for parent in parents:
            arguments += ['-p', parent]
        return self.git(*arguments)

    def git_commit(self, sha):
        parents = self.git('rev-list', '--parents', '-n', '1', sha).split()[1:]
        return {'sha': sha, 'tree': {'sha': self.git('rev-parse', f'{sha}^{{tree}}')},
                'parents': [{'sha': parent} for parent in parents]}

    def api(self, method, path, payload=None):
        """Emulate only the Git API with local Git, with no remote mutations."""
        self.calls.append((method, path, payload))
        prefix = f'/repos/{controller.repository}/git/'
        if method == 'GET' and path.startswith(prefix + 'commits/'):
            return self.git_commit(path.rsplit('/', 1)[1])
        if method == 'POST' and path == prefix + 'trees':
            # Follow base_tree supplied by the controller, rather than return
            # an expected tree unconditionally. This catches the old bug.
            entries = self.git('ls-tree', '-z', payload['base_tree']).split('\0')
            entries = [entry for entry in entries if entry and entry.split('\t')[1] != '.github']
            overlay, = payload['tree']
            self.assertEqual(overlay['path'], '.github')
            entries.append(f"040000 tree {overlay['sha']}\t.github")
            return {'sha': self.git('mktree', '-z', input='\0'.join(entries) + '\0')}
        if method == 'GET' and path == prefix + 'ref/heads/' + self.branch:
            if self.release:
                return {'object': {'sha': self.release}}
            raise HTTPError(path, 404, 'not found', {}, None)
        if method == 'POST' and path == prefix + 'commits':
            sha = self.commit_tree(payload['tree'], payload['parents'])
            return self.git_commit(sha)
        if method == 'POST' and path == prefix + 'refs':
            self.release = payload['sha']
            self.git('push', '-q', 'origin', f"{self.release}:{payload['ref']}")
            return {'object': {'sha': self.release}}
        raise AssertionError((method, path, payload))

    def make_release(self):
        tree = controller.expected_release_tree(self.merge)
        return self.commit_tree(tree, [self.merge])

    def test_staging_preserves_master_only_feature_and_exact_integration_parent(self):
        with patch.object(controller, 'api', side_effect=self.api), \
                patch.object(controller, 'dispatch') as dispatch:
            controller.create_release_candidate(self.pr, self.merge, self.baseline)
        self.assertEqual(self.git('show', f'{self.release}:master-only-feature'),
                         'previously reviewed stable AppImage updater')
        self.assertEqual(self.git('show', f'{self.release}:upstream-feature'), 'new upstream release')
        self.assertEqual(self.git_commit(self.release)['parents'], [{'sha': self.merge}])
        controller.verify_release_tree(self.merge, self.release, self.baseline)
        self.assertEqual(dispatch.call_count, 2)
        tree_request = next(payload for method, path, payload in self.calls
                            if method == 'POST' and path.endswith('/trees'))
        self.assertEqual(tree_request['base_tree'], self.git('rev-parse', f'{self.merge}^{{tree}}'))

    def test_identical_trusted_overlay_is_valid_and_staging_is_idempotent(self):
        with patch.object(controller, 'api', side_effect=self.api), \
                patch.object(controller, 'dispatch'):
            controller.create_release_candidate(self.pr, self.merge, self.baseline)
            first = self.release
            controller.create_release_candidate(self.pr, self.merge, self.baseline)
        self.assertEqual(self.release, first)
        self.assertEqual(self.git('diff', '--name-only', f'{self.merge}..{self.release}'), '')
        self.assertEqual(sum(method == 'POST' and path.endswith('/commits')
                             for method, path, _ in self.calls), 1)

    def test_existing_release_from_pr_head_cannot_be_reused(self):
        self.release = self.commit_tree(controller.expected_release_tree(self.candidate),
                                        [self.candidate])
        with patch.object(controller, 'api', side_effect=self.api), \
                patch.object(controller, 'dispatch') as dispatch, \
                self.assertRaisesRegex(RuntimeError, 'Refusing to reuse'):
            controller.create_release_candidate(self.pr, self.merge, self.baseline)
        dispatch.assert_not_called()

    def test_merge_requires_exact_ordered_parents_and_source(self):
        tree = self.git('rev-parse', f'{self.merge}^{{tree}}')
        for parents in ([self.base], [self.candidate, self.base],
                        [self.base, self.upstream], [self.fork, self.candidate],
                        [self.base, self.candidate, self.upstream]):
            with self.subTest(parents=parents):
                bad = self.commit_tree(tree, parents)
                self.git('push', '-q', '--force', 'origin', f'{bad}:refs/heads/master')
                with self.assertRaisesRegex(RuntimeError, 'reviewed base and source'):
                    controller.verify_reviewed_merge(self.pr, bad, self.baseline,
                                                       expected_base=self.base)

    def test_merge_with_right_parents_but_wrong_tree_is_rejected(self):
        wrong_tree = self.git('rev-parse', f'{self.candidate}^{{tree}}')
        bad = self.commit_tree(wrong_tree, [self.base, self.candidate])
        self.git('push', '-q', '--force', 'origin', f'{bad}:refs/heads/master')
        with patch.object(controller, 'api') as api, \
                self.assertRaisesRegex(RuntimeError, 'tree differs'):
            controller.create_release_candidate(self.pr, bad, self.baseline)
        api.assert_not_called()

    def test_integration_must_remain_on_default_branch_but_may_have_descendants(self):
        self.git('push', '-q', '--force', 'origin', f'{self.base}:refs/heads/master')
        with self.assertRaisesRegex(RuntimeError, 'no longer on the default branch'):
            controller.verify_reviewed_merge(self.pr, self.merge, self.baseline)
        descendant = self.commit_tree(self.git('rev-parse', f'{self.merge}^{{tree}}'), [self.merge])
        self.git('push', '-q', 'origin', f'{descendant}:refs/heads/master')
        self.assertEqual(controller.verify_reviewed_merge(self.pr, self.merge, self.baseline),
                         self.base)

    def test_release_with_correct_parent_cannot_omit_master_only_feature(self):
        wrong_tree = controller.expected_release_tree(self.candidate)
        release = self.commit_tree(wrong_tree, [self.merge])
        with self.assertRaisesRegex(RuntimeError, 'Release tree differs'):
            controller.verify_release_tree(self.merge, release, self.baseline)

    def test_release_rejects_wrong_parent_extra_source_change_and_stale_overlay(self):
        good_tree = controller.expected_release_tree(self.merge)
        for parents in ([], [self.candidate], [self.merge, self.candidate]):
            with self.subTest(parents=parents):
                wrong_parent = self.commit_tree(good_tree, parents)
                with self.assertRaisesRegex(RuntimeError, 'single child'):
                    controller.verify_release_tree(self.merge, wrong_parent, self.baseline)
        self.git('checkout', '-q', '--detach', self.merge)
        self.write('application', 'unreviewed release-only change\n')
        bad_application = self.commit('unreviewed application edit')
        with self.assertRaisesRegex(RuntimeError, 'Release tree differs'):
            controller.verify_release_tree(self.merge, bad_application, self.baseline)
        self.git('checkout', '-q', '--detach', self.merge)
        self.write('.github/controller', 'stale or untrusted controller\n')
        stale = self.commit('stale overlay')
        self.git('checkout', '-q', '--detach', self.base)
        with self.assertRaisesRegex(RuntimeError, 'Release tree differs'):
            controller.verify_release_tree(self.merge, stale, self.baseline)
        good_release = self.make_release()
        self.write('.github/controller', 'newer trusted controller\n')
        self.commit('trusted controller advanced after verification')
        with self.assertRaisesRegex(RuntimeError, 'Release tree differs'):
            controller.verify_release_tree(self.merge, good_release, self.baseline)

    def test_changed_file_mode_is_rejected_even_with_identical_contents(self):
        self.git('checkout', '-q', '--detach', self.merge)
        self.git('update-index', '--chmod=+x', 'application')
        release = self.commit_tree(self.git('write-tree'), [self.merge])
        with self.assertRaisesRegex(RuntimeError, 'Release tree differs'):
            controller.verify_release_tree(self.merge, release, self.baseline)

    def test_owner_authored_integrated_pr_remains_ineligible(self):
        owner_pr = dict(self.pr, user={'login': 'feiyang-cai'})
        with patch.object(controller, 'api') as api, \
                self.assertRaisesRegex(RuntimeError, 'not authored by Copilot'):
            controller.validate_sync_metadata(owner_pr, self.baseline)
        api.assert_not_called()
        integrated = dict(self.pr, head=dict(self.pr['head'], sha=self.merge))
        self.git('push', '-q', '--force', 'origin', f'{self.merge}:refs/pull/42/head')
        with self.assertRaisesRegex(RuntimeError, 'not based exactly'):
            controller.fetch_candidate(integrated)

    def test_promotion_verifies_integration_before_tagging(self):
        self.release = self.make_release()
        self.git('push', '-q', 'origin', f'{self.release}:refs/heads/{self.branch}')
        run = {'head_sha': self.release, 'head_branch': self.branch}
        with patch.object(controller, 'api', side_effect=self.api), \
                patch.object(controller, 'workflows_pass', return_value=True) as checks, \
                patch.object(controller, 'sync_pr_for_commit', return_value=self.pr) as lookup, \
                patch.object(controller, 'resolve_upstream_tag', return_value=self.upstream), \
                patch.object(controller, 'validate_sync_metadata') as metadata, \
                patch.object(controller, 'ai_review_passes', return_value=True) as review, \
                patch.object(controller, 'create_release_tag') as tag:
            controller.promote_release_candidate(run, self.baseline, dry_run=True)
        lookup.assert_called_once_with(self.merge)
        review.assert_called_once_with(self.pr, self.base)
        metadata.assert_called_once_with(self.pr, self.baseline)
        checks.assert_any_call(self.candidate, {'push'})
        tag.assert_called_once_with(self.pr, self.merge, self.release, self.baseline, True)

    def test_promotion_rejects_wrong_pr_merge_and_missing_master_source(self):
        for mismatch in (False, True):
            with self.subTest(wrong_pr_merge=mismatch):
                tree = controller.expected_release_tree(self.candidate)
                self.release = self.commit_tree(tree, [self.merge])
                self.git('push', '-q', '--force', 'origin', f'{self.release}:refs/heads/{self.branch}')
                pr = dict(self.pr, merge_commit_sha=self.candidate) if mismatch else self.pr
                run = {'head_sha': self.release, 'head_branch': self.branch}
                with patch.object(controller, 'api', side_effect=self.api), \
                        patch.object(controller, 'workflows_pass', return_value=True), \
                        patch.object(controller, 'sync_pr_for_commit', return_value=pr), \
                        patch.object(controller, 'resolve_upstream_tag', return_value=self.upstream), \
                        patch.object(controller, 'validate_sync_metadata'), \
                        patch.object(controller, 'ai_review_passes', return_value=True), \
                        patch.object(controller, 'create_release_tag') as tag, \
                        self.assertRaisesRegex(RuntimeError, 'integration source|Release tree differs'):
                    controller.promote_release_candidate(run, self.baseline)
                tag.assert_not_called()

    def test_publication_rejects_missing_master_source_before_recording_progress(self):
        tree = controller.expected_release_tree(self.candidate)
        release = self.commit_tree(tree, [self.merge])
        self.git('push', '-q', 'origin', f'{release}:refs/heads/{self.branch}')
        run = {'head_sha': release, 'head_branch': 'texstudio-vim-5.0.0-r1.1'}
        with patch.object(controller, 'sync_pr_for_release_commit', return_value=self.pr), \
                patch.object(controller, 'validate_sync_metadata'), \
                patch.object(controller, 'ai_review_passes', return_value=True), \
                patch.object(controller, 'api', return_value={
                    'object': {'type': 'commit', 'sha': release}}) as api, \
                self.assertRaisesRegex(RuntimeError, 'Release tree differs'):
            controller.publish_progress(run, self.baseline)
        self.assertTrue(all(call.args[0] == 'GET' for call in api.call_args_list))

    def test_publication_records_exact_integration_release_only_after_review(self):
        release = self.make_release()
        self.git('push', '-q', 'origin', f'{release}:refs/heads/{self.branch}')
        tag = 'texstudio-vim-5.0.0-r1.1'
        run = {'head_sha': release, 'head_branch': tag}
        calls = []
        state = {'stable': {'upstream_tag': '4.9.9'}, 'prerelease': {}}

        def api(method, path, payload=None):
            calls.append((method, path, payload))
            if path.endswith('/git/ref/tags/' + tag):
                return {'object': {'type': 'commit', 'sha': release}}
            if path.endswith('/releases/tags/' + tag):
                return {'draft': False, 'prerelease': False}
            if path.endswith('/releases/tags/5.0.0'):
                return {'draft': False, 'prerelease': False, 'published_at': '2026-11-01T00:00:00Z'}
            if path.endswith('/git/ref/tags/5.0.0'):
                return {'object': {'type': 'commit', 'sha': self.upstream}}
            if '/contents/.github/upstream-release-state.json' in path:
                if method == 'GET':
                    return {'content': base64.b64encode(json.dumps(state).encode()).decode(),
                            'sha': 'fixture-state-sha'}
                self.assertEqual(method, 'PUT')
                return {}
            raise AssertionError((method, path, payload))

        with patch.object(controller, 'sync_pr_for_release_commit', return_value=self.pr), \
                patch.object(controller, 'validate_sync_metadata') as metadata, \
                patch.object(controller, 'ai_review_passes', return_value=True) as review, \
                patch.object(controller, 'api', side_effect=api):
            controller.publish_progress(run, self.baseline)
        metadata.assert_called_once_with(self.pr, self.baseline)
        review.assert_called_once_with(self.pr, self.base)
        writes = [payload for method, _, payload in calls if method == 'PUT']
        self.assertEqual(len(writes), 1)
        published = json.loads(base64.b64decode(writes[0]['content']))
        self.assertEqual(published['stable']['fork_sha'], release)
        self.assertEqual(published['stable']['fork_tag'], tag)
        self.assertEqual(published['stable']['upstream_sha'], self.upstream)
        self.assertEqual(published['prerelease'], state['prerelease'])

    def test_publication_missing_review_cannot_record_progress(self):
        release = self.make_release()
        self.git('push', '-q', 'origin', f'{release}:refs/heads/{self.branch}')
        run = {'head_sha': release, 'head_branch': 'texstudio-vim-5.0.0-r1.1'}
        with patch.object(controller, 'sync_pr_for_release_commit', return_value=self.pr), \
                patch.object(controller, 'validate_sync_metadata'), \
                patch.object(controller, 'ai_review_passes', return_value=False) as review, \
                patch.object(controller, 'api', return_value={
                    'object': {'type': 'commit', 'sha': release}}) as api, \
                self.assertRaisesRegex(RuntimeError, 'no longer has approval'):
            controller.publish_progress(run, self.baseline)
        review.assert_called_once_with(self.pr, self.base)
        self.assertTrue(all(call.args[0] == 'GET' for call in api.call_args_list))

    def test_conflicting_reviewed_inputs_never_stage_a_release(self):
        self.write('application', 'conflicting base change\n')
        base = self.commit('conflicting base')
        self.git('checkout', '-q', '--detach', self.candidate)
        self.write('application', 'conflicting candidate change\n')
        head = self.commit('conflicting candidate')
        merge = self.commit_tree(self.git('rev-parse', f'{head}^{{tree}}'), [base, head])
        self.git('push', '-q', '--force', 'origin', f'{merge}:refs/heads/master')
        pr = dict(self.pr, base={'sha': base, 'ref': 'master'},
                  head=dict(self.pr['head'], sha=head))
        with patch.object(controller, 'api') as api, \
                self.assertRaisesRegex(RuntimeError, 'conflict-free reviewed merge tree'):
            controller.create_release_candidate(pr, merge, self.baseline)
        api.assert_not_called()

    def test_master_only_changes_require_human_even_for_pure_auto_source(self):
        self.assertTrue(controller.integration_changes_need_human(self.pr, self.base))
        # Human review is tied to the exact source head after the trusted base
        # snapshot, not to a claim that master changes were previously approved.
        snapshot = {'description': self.base, 'created_at': '2026-11-01T00:00:00Z'}
        review = {'state': 'APPROVED', 'submitted_at': '2026-11-01T01:00:00Z'}
        for human_approved in (False, True):
            with self.subTest(human_approved=human_approved), \
                    patch.object(controller, 'latest_ai_review', return_value=review), \
                    patch.object(controller, 'review_base_snapshot', return_value=snapshot), \
                    patch.object(controller, 'unresolved_ai_threads', return_value=False), \
                    patch.object(controller, 'upstream_changes_need_human', return_value=False), \
                    patch.object(controller, 'human_validation_passes',
                                 return_value=human_approved) as human:
                self.assertEqual(controller.ai_review_passes(self.pr), human_approved)
                self.assertEqual(controller.ai_review_passes(self.pr, self.base), human_approved)
                human.assert_called_with(self.pr, snapshot)

    def test_controls_only_base_changes_do_not_invent_human_source_review(self):
        self.git('checkout', '-q', '--detach', self.fork)
        self.write('.github/controller', 'new trusted controls only\n')
        base = self.commit('controls-only change')
        self.git('push', '-q', 'origin', f'{base}:refs/heads/controls-only')
        self.assertFalse(controller.integration_changes_need_human(self.pr, base))

    def test_base_only_mode_change_also_requires_human_review(self):
        self.git('checkout', '-q', '--detach', self.fork)
        self.git('update-index', '--chmod=+x', 'application')
        base = self.commit_tree(self.git('write-tree'), [self.fork])
        self.git('push', '-q', 'origin', f'{base}:refs/heads/mode-change')
        self.assertTrue(controller.integration_changes_need_human(self.pr, base))

    def test_integrated_and_release_vim_revisions_must_remain_approved(self):
        self.write('VIM_REVISION', '1.2\n')
        base = self.commit('unapproved base-only Vim development')
        tree = self.git('merge-tree', '--write-tree', base, self.candidate)
        merge = self.commit_tree(tree, [base, self.candidate])
        self.git('push', '-q', '--force', 'origin', f'{merge}:refs/heads/master')
        pr = dict(self.pr, base={'sha': base, 'ref': 'master'})
        with self.assertRaisesRegex(RuntimeError, 'approved VIM_REVISION'):
            controller.integration_changes_need_human(pr, base)
        with patch.object(controller, 'api') as api, \
                self.assertRaisesRegex(RuntimeError, 'approved VIM_REVISION'):
            controller.create_release_candidate(pr, merge, self.baseline)
        api.assert_not_called()
        release = self.commit_tree(tree, [self.merge])
        with self.assertRaisesRegex(RuntimeError, 'approved VIM_REVISION'):
            controller.verify_release_tree(self.merge, release, self.baseline)

    def test_crlf_line_endings_are_allowed_but_actual_whitespace_errors_fail(self):
        self.write('completion.cwl', 'original\n')
        base = self.commit('whitespace baseline')
        for contents, allowed in (
                (b'updated\n', True), (b'updated\r\n', True),
                (b'updated \n', False), (b'updated \r\n', False),
                (b'updated\t\n', False), (b'updated\t\r\n', False),
                (b' \tindented\n', False), (b' \tindented\r\n', False),
                (b'updated\n\n', False), (b'updated\r\n\r\n', False)):
            with self.subTest(contents=contents):
                (self.work / 'completion.cwl').write_bytes(contents)
                sha = self.commit('whitespace candidate')
                if allowed:
                    controller.verify_candidate_whitespace(base, sha)
                else:
                    with self.assertRaisesRegex(RuntimeError, 'whitespace errors'):
                        controller.verify_candidate_whitespace(base, sha)


if __name__ == '__main__':
    unittest.main()
