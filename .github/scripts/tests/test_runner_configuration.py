"""Keep supported desktop runners and strict release gates in agreement."""
import ast
import importlib.util
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    'runner_controller', ROOT / '.github/scripts/upstream-release-controller.py')
controller = importlib.util.module_from_spec(spec)
spec.loader.exec_module(controller)

DESKTOP_RUNNERS = {
    'windows-latest', 'windows-11-arm', 'macos-15-intel', 'macos-15',
}
LINUX_DISTROS = {'ubuntu-22.04', 'ubuntu-24.04', 'debian-12', 'debian-13', 'fedora-43'}


def macos_runner_labels(source):
    # A helper path such as install-macos-remote-test-deps.sh is not a runner.
    # Keep unknown/retired standalone labels visible to the strict set check.
    return set(re.findall(r'(?<![\w/-])macos-[\w-]+(?![\w/-])', source))


class RunnerConfigurationTests(unittest.TestCase):
    def test_active_workflows_keep_both_mac_architectures_on_supported_runners(self):
        for filename in ('cd.yml', 'vim-platforms.yml', 'packaged-gui.yml'):
            with self.subTest(workflow=filename):
                source = (ROOT / '.github/workflows' / filename).read_text()
                self.assertEqual(macos_runner_labels(source),
                                 {'macos-15', 'macos-15-intel'})

    def test_runner_labels_exclude_helper_paths_without_hiding_retired_runners(self):
        source = ('runner: [macos-15, macos-15-intel, macos-14, macos-retired]\n'
                  'run: bash .github/scripts/install-macos-remote-test-deps.sh\n')
        self.assertEqual(macos_runner_labels(source),
                         {'macos-15', 'macos-15-intel', 'macos-14', 'macos-retired'})

    def test_controller_still_requires_all_four_native_desktops(self):
        self.assertEqual(controller.VIM_JOBS,
                         {f'Vim desktop ({runner})' for runner in DESKTOP_RUNNERS})

    def test_controller_still_requires_all_nine_packaged_desktops(self):
        expected = {f'Packaged GUI ({runner})' for runner in DESKTOP_RUNNERS}
        expected |= {f'AppImage desktop ({distro})' for distro in LINUX_DISTROS}
        actual = {name for name in controller.CD_JOBS
                  if name.startswith(('Packaged GUI (', 'AppImage desktop ('))}
        self.assertEqual(actual, expected)
        self.assertEqual(len(actual), 9)

    def test_recovery_requires_current_native_runners_without_execution(self):
        source = (ROOT / '.github/scripts/read-release-request.py').read_text()
        tree = ast.parse(source)
        assignments = [node for node in ast.walk(tree)
                       if isinstance(node, ast.Assign)
                       and any(isinstance(target, ast.Name) and target.id == 'required_gui'
                               for target in node.targets)]
        self.assertEqual(len(assignments), 1)
        expression = assignments[0].value
        self.assertIsInstance(expression, ast.SetComp)
        self.assertEqual(set(ast.literal_eval(expression.generators[0].iter)),
                         DESKTOP_RUNNERS)

    def test_publication_recording_can_recheck_reviews_and_merge_history(self):
        source = (ROOT / '.github/workflows/upstream-release-sync.yml').read_text()
        self.assertEqual(source.count('\n  publish:\n'), 1)
        publish = source.split('\n  publish:\n', 1)[1]
        self.assertIn('\n      pull-requests: read\n', publish)
        self.assertIn('\n      statuses: read\n', publish)
        self.assertIn('\n          fetch-depth: 0\n', publish)
        self.assertNotIn('pull-requests: write', publish)
        self.assertNotIn('statuses: write', publish)


if __name__ == '__main__':
    unittest.main()
