import importlib.util
from pathlib import Path
import unittest
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

    def test_release_progress_markers_keep_streams_separate(self):
        sha = 'a' * 40
        self.assertEqual(detector.sync_marker('stable', '5.0.0', sha),
                         f'<!-- upstream-release-sync:stable:5.0.0:{sha} -->')
        self.assertEqual(detector.sync_marker('prerelease', '5.0.0beta1', sha),
                         f'<!-- upstream-release-sync:prerelease:5.0.0beta1:{sha} -->')

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

    def test_controller_parses_only_complete_sync_markers(self):
        self.assertEqual(controller.marker_from(
            'Sync release\n<!-- upstream-release-sync:stable:5.0.0:' + 'a' * 40 + ' -->'),
            ('stable', '5.0.0', 'a' * 40))
        self.assertEqual(controller.marker_from(
            '<!-- upstream-release-sync:prerelease:5.0.0beta1:' + 'b' * 40 + ' -->'),
            ('prerelease', '5.0.0beta1', 'b' * 40))
        self.assertIsNone(controller.marker_from(
            '<!-- upstream-release-sync:unknown:5.0.0 -->'))

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
        files = [{'filename': 'src/latexeditorview.cpp'},
                 {'filename': '.github/workflows/upstream-release-sync.yml'}]
        self.assertEqual(controller.changed_release_controls(files),
                         ['.github/workflows/upstream-release-sync.yml'])

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


if __name__ == '__main__':
    unittest.main()
