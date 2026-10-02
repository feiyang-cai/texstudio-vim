"""Check branch builds and exact-tag Windows builds against their package provenance."""
import re
from pathlib import Path

def package_identity(version, expected_sha, archive_name):
    if not re.fullmatch(r'[0-9a-f]{40}', expected_sha):
        raise ValueError('Expected a full source commit SHA')
    commit = re.search(r'-g([0-9a-f]{7,40})\)\s*$', version)
    if commit and expected_sha.startswith(commit.group(1)):
        return 'binary commit suffix'
    # The Windows MSYS workaround intentionally emits a pure tag at a release.
    tag = re.fullmatch(r'TeXstudio ([0-9]+\.[0-9]+\.[0-9]+) \((texstudio-vim-([0-9]+\.[0-9]+\.[0-9]+)(?:(?:alpha|beta|rc)[0-9]+)?-r[0-9]+)\)\s*', version)
    archive = Path(archive_name).name
    archive_commit = re.search(r'-git_([0-9a-f]{7,40})\.zip$', archive)
    if (tag and tag.group(1) == tag.group(3) and archive_commit
            and expected_sha.startswith(archive_commit.group(1))
            and '-' + tag.group(2).removeprefix('texstudio-vim-') + '-' in archive):
        return 'canonical release tag and archive commit'
    raise ValueError('Binary version and package provenance do not match the expected source')
