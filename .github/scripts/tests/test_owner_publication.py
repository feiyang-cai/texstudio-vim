import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('owner_publisher', ROOT / '.github/scripts/publish-owner-approved-release.py')
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


class OwnerPublicationTests(unittest.TestCase):
    def test_draft_lookup_uses_authenticated_release_listing(self):
        draft = {'tag_name': 'target', 'draft': True}
        with patch.object(publisher, 'api', return_value=[{'tag_name': 'other'}, draft]) as api:
            self.assertEqual(publisher.release_for_tag('repos/test/test', 'target'), draft)
            api.assert_called_once_with('repos/test/test/releases?per_page=100')
        with patch.object(publisher, 'api', return_value=[draft, draft]):
            with self.assertRaises(RuntimeError):
                publisher.release_for_tag('repos/test/test', 'target')

    def test_requires_successful_exact_tag_and_all_named_checks(self):
        sha, tag = 'a' * 40, 'texstudio-vim-4.9.9-r1.1'
        run = {'head_sha': sha, 'head_branch': tag, 'event': 'push',
               'head_repository': {'full_name': 'feiyang-cai/texstudio-vim'},
               'conclusion': 'success'}
        jobs = [{'name': name, 'conclusion': 'success'} for name in publisher.CD_JOBS]
        publisher.verify_jobs(run, jobs, publisher.CD_JOBS, sha, tag)
        for invalid in (dict(run, head_sha='b' * 40), dict(run, head_branch='master'),
                        dict(run, conclusion='failure'), dict(run, event='pull_request')):
            with self.assertRaises(RuntimeError):
                publisher.verify_jobs(invalid, jobs, publisher.CD_JOBS, sha, tag)
        with self.assertRaises(RuntimeError):
            publisher.verify_jobs(run, jobs[:-1], publisher.CD_JOBS, sha, tag)
        jobs[0]['conclusion'] = 'skipped'
        with self.assertRaises(RuntimeError):
            publisher.verify_jobs(run, jobs, publisher.CD_JOBS, sha, tag)
        self.assertEqual(len(publisher.CD_JOBS), 15)
        self.assertEqual(len(publisher.VIM_JOBS), 4)

    def test_requires_complete_uploaded_assets_with_expected_sizes_and_digests(self):
        tag = 'texstudio-vim-4.9.9-r1.1'
        expected = {'test.AppImage': (123, 'abc')}
        asset = {'name': 'test.AppImage', 'size': 123, 'digest': 'sha256:abc', 'state': 'uploaded'}
        release = {'tag_name': tag, 'prerelease': False, 'assets': [asset]}
        publisher.verify_assets(release, expected, tag)
        for changed in ({'size': 122}, {'digest': 'sha256:wrong'}, {'digest': None}, {'state': 'new'}, {'name': 'wrong'}):
            with self.assertRaises(RuntimeError):
                publisher.verify_assets(dict(release, assets=[dict(asset, **changed)]), expected, tag)
        for changed in ({'assets': []}, {'prerelease': True}, {'tag_name': 'wrong'}):
            with self.assertRaises(RuntimeError):
                publisher.verify_assets(dict(release, **changed), expected, tag)

    def test_only_the_owner_requested_stable_release_is_allowed(self):
        import json
        request = json.loads((ROOT / '.github/manual-release-request.json').read_text())
        publisher.validate_request(request)
        for changed in ({'tag': 'texstudio-vim-4.9.9rc1-r1.1'}, {'repository': 'other/repo'},
                        {'vim_revision': '2'}, {'upstream_tag': '4.10.0'}, {'sha': 'invalid'}):
            with self.assertRaises(RuntimeError):
                publisher.validate_request(dict(request, **changed))
