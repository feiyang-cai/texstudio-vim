import importlib.util
import json
from pathlib import Path
import unittest
from urllib.error import HTTPError
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]


def load_script(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


detector = load_script(
    'detect_upstream_releases', '.github/scripts/detect-upstream-releases.py')
controller = load_script(
    'upstream_release_controller', '.github/scripts/upstream-release-controller.py')


class UpstreamReleaseSyncTests(unittest.TestCase):
    def test_release_classification_ignores_drafts_and_non_version_tags(self):
        self.assertIsNone(detector.classify_release({
            'draft': True, 'prerelease': False, 'tag_name': '5.0.0'}))
        self.assertIsNone(detector.classify_release({
            'draft': False, 'prerelease': False, 'tag_name': 'latest'}))
        self.assertEqual(detector.classify_release({
            'draft': False, 'prerelease': False, 'tag_name': '5.0.0'}), 'stable')
        self.assertEqual(detector.classify_release({
            'draft': False, 'prerelease': True, 'tag_name': '5.0.0rc1'}), 'prerelease')

    def test_release_progress_markers_pin_upstream_and_fork_bases(self):
        sha = 'a' * 40
        fork_base = 'b' * 40
        self.assertEqual(detector.sync_marker('stable', '5.0.0', sha, fork_base),
                         f'<!-- upstream-release-sync:stable:5.0.0:{sha}:{fork_base} -->')
        self.assertEqual(detector.sync_marker('prerelease', '5.0.0beta1', sha, fork_base),
                         f'<!-- upstream-release-sync:prerelease:5.0.0beta1:{sha}:{fork_base} -->')

    def test_detector_ignores_handled_releases_and_selects_oldest_new_release(self):
        releases = [
            {'draft': False, 'prerelease': False, 'tag_name': '5.0.2',
             'published_at': '2026-10-03T00:00:00Z'},
            {'draft': True, 'prerelease': False, 'tag_name': '5.0.1',
             'published_at': '2026-10-02T00:00:00Z'},
            {'draft': False, 'prerelease': False, 'tag_name': '5.0.1',
             'published_at': '2026-10-02T00:00:00Z'},
            {'draft': False, 'prerelease': False, 'tag_name': '5.0.0',
             'published_at': '2026-10-01T00:00:00Z'},
            {'draft': False, 'prerelease': True, 'tag_name': '5.0.1beta1',
             'published_at': '2026-10-02T12:00:00Z'},
        ]
        progress = {'upstream_tag': '5.0.0', 'published_at': '2026-10-01T00:00:00Z'}
        self.assertEqual(detector.next_release(releases, 'stable', progress)['tag_name'], '5.0.1')
        self.assertEqual(detector.next_release(releases, 'prerelease', progress)['tag_name'],
                         '5.0.1beta1')

    def test_dry_run_prevents_sync_issue_mutations(self):
        self.assertFalse(detector.sync_issue_mutations_enabled({'dry_run': True}))
        self.assertFalse(detector.sync_issue_mutations_enabled({}))
        self.assertTrue(detector.sync_issue_mutations_enabled({'dry_run': False}))

    def test_controller_parses_only_complete_sync_markers(self):
        self.assertEqual(controller.marker_from(
            'Sync release\n<!-- upstream-release-sync:stable:5.0.0:' + 'a' * 40 +
            ':' + 'b' * 40 + ' -->'),
            ('stable', '5.0.0', 'a' * 40, 'b' * 40))
        self.assertEqual(controller.marker_from(
            '<!-- upstream-release-sync:prerelease:5.0.0beta1:' + 'b' * 40 +
            ':' + 'c' * 40 + ' -->'),
            ('prerelease', '5.0.0beta1', 'b' * 40, 'c' * 40))
        self.assertIsNone(controller.marker_from(
            '<!-- upstream-release-sync:unknown:5.0.0 -->'))

    def test_dry_run_and_publication_switch_fail_closed_by_default(self):
        policy = {'dry_run': True, 'publication_enabled': False}
        self.assertFalse(controller.publication_enabled(policy))
        self.assertFalse(controller.publication_enabled({
            'dry_run': False, 'publication_enabled': False}))
        with patch.dict(controller.os.environ, {}, clear=True):
            self.assertFalse(controller.publication_enabled({
                'dry_run': False, 'publication_enabled': True}))
        with patch.dict(controller.os.environ, {'UPSTREAM_RELEASE_PUBLICATION_ENABLED': 'false'}):
            self.assertFalse(controller.publication_enabled({
                'dry_run': False, 'publication_enabled': True}))
        with patch.dict(controller.os.environ, {'UPSTREAM_RELEASE_PUBLICATION_ENABLED': 'true'}):
            self.assertTrue(controller.publication_enabled({
                'dry_run': False, 'publication_enabled': True}))
        checked_in = json.loads(
            (ROOT / '.github/upstream-release-policy.json').read_text())
        self.assertTrue(checked_in['dry_run'])
        self.assertFalse(checked_in['publication_enabled'])

    def test_dry_run_never_creates_release_tag_refs(self):
        upstream_sha, fork_base, candidate = 'a' * 40, 'b' * 40, 'c' * 40
        pr = {
            'body': detector.sync_marker('stable', '5.0.0', upstream_sha, fork_base),
            'head': {'sha': candidate},
        }
        missing_tag = HTTPError('https://api.github.com/ref', 404, 'not found', {}, None)
        with patch.object(controller, 'branch_sha', return_value='d' * 40), \
                patch.object(controller, 'resolve_upstream_tag', return_value=upstream_sha), \
                patch.object(controller.subprocess, 'run',
                             return_value=type('Result', (), {'returncode': 0})()), \
                patch.object(controller, 'api', side_effect=missing_tag) as api:
            controller.create_release_tag(
                pr, 'd' * 40, {'branch': 'master', 'vim_revision': 1}, dry_run=True)
        self.assertFalse(any(call.args[0] != 'GET' for call in api.call_args_list))

    def test_missing_or_skipped_required_jobs_do_not_pass(self):
        run = {'id': 1, 'status': 'completed', 'conclusion': 'success'}
        with patch.object(controller, 'api', return_value={'jobs': [
                {'name': 'required', 'conclusion': 'success'},
                {'name': 'skipped', 'conclusion': 'skipped'}]}):
            self.assertFalse(controller.require_job_set(run, {'required', 'skipped'}))
            self.assertTrue(controller.require_job_set(run, {'required'}))

    def test_workflow_failure_does_not_pass_even_with_successful_steps(self):
        run = {'id': 1, 'status': 'completed', 'conclusion': 'failure'}
        self.assertFalse(controller.require_job_set(run, set()))

    def test_release_control_edits_are_rejected(self):
        files = [
            {'filename': 'src/latexeditorview.cpp'},
            {'filename': '.github/workflows/upstream-release-sync.yml'},
            {'filename': '.github/scripts/package_identity.py'},
            {'filename': '.github/scripts/smoke-appimage.py'},
            {'filename': '.github/actions/release/action.yml'},
            {'filename': 'old-tests.py', 'previous_filename': '.github/scripts/tests/test_old.py'},
        ]
        self.assertEqual(controller.changed_release_controls(files), [
            '.github/workflows/upstream-release-sync.yml',
            '.github/scripts/package_identity.py',
            '.github/scripts/smoke-appimage.py',
            '.github/actions/release/action.yml',
            '.github/scripts/tests/test_old.py',
        ])

    def test_release_base_must_be_approved_first_parent(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            import subprocess
            subprocess.run(['git', 'init', '-q', directory], check=True)
            run = lambda *args: subprocess.run(
                ['git', '-C', directory, *args], check=True, capture_output=True, text=True)
            run('config', 'user.name', 'Test')
            run('config', 'user.email', 'test@example.invalid')
            (Path(directory) / 'base').write_text('approved\n')
            run('add', 'base')
            run('commit', '-qm', 'approved baseline')
            approved = run('rev-parse', 'HEAD').stdout.strip()
            run('branch', '-M', 'master')
            run('checkout', '-qb', 'sync')
            (Path(directory) / 'upstream').write_text('upstream\n')
            run('add', 'upstream')
            run('commit', '-qm', 'upstream release')
            candidate = run('rev-parse', 'HEAD').stdout.strip()
            first_parent = run('rev-parse', f'{candidate}^1').stdout.strip()
            self.assertEqual(first_parent, approved)
            self.assertTrue(controller.candidate_uses_published_base(first_parent, approved))
            run('checkout', '-q', 'master')
            (Path(directory) / 'unapproved').write_text('development\n')
            run('add', 'unapproved')
            run('commit', '-qm', 'unapproved master work')
            run('checkout', '-q', 'sync')
            (Path(directory) / 'upstream').write_text('upstream + unapproved\n')
            run('commit', '-qam', 'include unapproved master work')
            contaminated = run('rev-parse', 'HEAD').stdout.strip()
            contaminated_parent = run('rev-parse', f'{contaminated}^1').stdout.strip()
            self.assertFalse(controller.candidate_uses_published_base(
                contaminated_parent, approved))

    def test_candidate_must_be_exact_two_parent_pinned_merge(self):
        commit, fork_base, upstream = 'a' * 40, 'b' * 40, 'c' * 40
        self.assertTrue(controller.candidate_is_pinned_merge(
            [commit, fork_base, upstream], commit, fork_base, upstream))
        self.assertFalse(controller.candidate_is_pinned_merge(
            [commit, 'd' * 40, fork_base, upstream], commit, fork_base, upstream))

    def test_release_staging_may_only_overlay_trusted_github_controls(self):
        self.assertTrue(controller.trusted_overlay_only([
            '.github/scripts/run-vim-tests.py',
            '.github/workflows/cd.yml',
        ]))
        self.assertFalse(controller.trusted_overlay_only([
            '.github/workflows/cd.yml',
            'src/latexeditorview.cpp',
        ]))
        self.assertFalse(controller.trusted_overlay_only([]))

    def test_candidate_must_preserve_owner_approved_vim_revision(self):
        baseline = {'vim_revision': 1}
        self.assertTrue(controller.has_approved_revision('1\n', baseline))
        self.assertFalse(controller.has_approved_revision('2\n', baseline))

    def test_changed_or_closed_pull_request_head_is_rejected(self):
        pr = {'head': {'sha': 'a' * 40}}
        controller.check_pr_head_is_current(pr, {
            'state': 'open', 'head': {'sha': 'a' * 40}, 'mergeable': True})
        with self.assertRaises(RuntimeError):
            controller.check_pr_head_is_current(pr, {
                'state': 'open', 'head': {'sha': 'b' * 40}, 'mergeable': True})
        with self.assertRaises(RuntimeError):
            controller.check_pr_head_is_current(pr, {
                'state': 'closed', 'head': {'sha': 'a' * 40}, 'mergeable': True})

    def test_fork_release_tag_parser_keeps_revision(self):
        self.assertEqual(
            controller.FORK_RELEASE_TAG.fullmatch(
                'texstudio-vim-5.0.0beta2-r1').groups(),
            ('5.0.0beta2', '1'))
        self.assertIsNone(controller.FORK_RELEASE_TAG.fullmatch('texstudio-vim-5.0.0'))

    def test_required_release_gates_cover_all_packages_and_gui_environments(self):
        self.assertTrue({
            'verify / Linux-autoTests',
            'win10 build (msys2)',
            'win10 build (msys2,arm)',
            'linux appimage',
            'Mac OS X',
            'Mac OS X (M1)',
        } <= controller.CD_JOBS)
        self.assertEqual(len(controller.CD_JOBS - {
            'verify / Linux-autoTests',
            'win10 build (msys2)',
            'win10 build (msys2,arm)',
            'linux appimage',
            'Mac OS X',
            'Mac OS X (M1)',
        }), 9)
        self.assertEqual(len(controller.VIM_JOBS), 4)

    def test_cd_release_job_and_assets_require_enable_switch(self):
        workflow = (ROOT / '.github/workflows/cd.yml').read_text()
        self.assertGreaterEqual(workflow.count(
            "vars.UPSTREAM_RELEASE_PUBLICATION_ENABLED == 'true'"), 6)
        release_job = workflow.split('  release:\n', 1)[1]
        self.assertIn("vars.UPSTREAM_RELEASE_PUBLICATION_ENABLED == 'true'", release_job)


if __name__ == '__main__':
    unittest.main()
