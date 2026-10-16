import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('package_identity', ROOT / '.github/scripts/package_identity.py')
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)


class ForkVersioningTests(unittest.TestCase):
    def test_windows_patch_release_provenance(self):
        sha = 'abcdef0123456789' * 2 + 'abcdef01'
        version = 'TeXstudio 4.9.9 (texstudio-vim-4.9.9rc1-r1.1)'
        archive = 'texstudio-vim-win-qt6-4.9.9rc1-r1.1-20261016-git_abcdef01.zip'
        self.assertEqual(identity.package_identity(version, sha, archive),
                         'canonical release tag and archive commit')
        with self.assertRaises(ValueError):
            identity.package_identity(version, sha, archive.replace('-r1.1-', '-r1.2-'))

    def test_package_names_follow_merged_upstream_and_source_vim_revision(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'src').mkdir()
            (root / 'build').mkdir()
            (root / 'src/utilsVersion.h').write_text('#define TXSVERSION "4.9.9"\n')
            (root / 'VIM_REVISION').write_text('1.1\n')
            def git(*args):
                return subprocess.run(['git', '-C', str(root), *args], check=True,
                                      capture_output=True, text=True)
            git('init', '-q')
            git('config', 'user.name', 'Version test')
            git('config', 'user.email', 'test@example.invalid')
            git('add', '.')
            git('commit', '-qm', 'fixture')
            git('tag', 'texstudio-vim-4.9.9beta2-r1')
            (root / 'upstream').touch()
            git('add', '.')
            git('commit', '-qm', 'upstream rc1')
            git('tag', 'upstream/4.9.9rc1')
            script = '. "$1" >/dev/null; printf "%s" "$GIT_VERSION"'
            env = dict(os.environ, GITHUB_REF_TYPE='branch')
            command = ['sh', '-ec', script, 'version-test', str(ROOT / '.github/scripts/get-version.sh')]
            result = subprocess.run(command, cwd=root / 'build', env=env,
                                    check=True, capture_output=True, text=True)
            self.assertEqual(result.stdout, '4.9.9rc1-r1.1')
            env.update(GITHUB_REF_TYPE='tag', GITHUB_REF_NAME='texstudio-vim-4.9.9rc1-r1.1')
            result = subprocess.run(command, cwd=root / 'build', env=env,
                                    check=True, capture_output=True, text=True)
            self.assertEqual(result.stdout, '4.9.9rc1-r1.1')
            (root / 'VIM_REVISION').write_text('1.1.1\n')
            result = subprocess.run(command, cwd=root / 'build', env=env,
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
