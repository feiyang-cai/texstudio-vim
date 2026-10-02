# Upstream sync and fork releases

The fork preserves native Vim editing on two upstream release bases:

| Fork tag | Upstream tag | Upstream commit |
| --- | --- | --- |
| `texstudio-vim-4.9.8-r0` | `4.9.8` | `00f7c1c3db1d629a854ff5eb8ee25eb402c8178e` |
| `texstudio-vim-4.9.9beta2-r0` | `4.9.9beta2` | `263f2615005cdbe5c24d4ee9a7c5746f731e25ac` |

4.9.8 is a stable upstream base; the fork's Vim implementation remains experimental.
4.9.9beta2 is a prerelease. Both include the features and known gaps in
[VIM_MODE.md](VIM_MODE.md).

## Safe sync

Start with a clean checkout and create a backup branch at the current fork commit.
Fetch the specific upstream tag into a separate namespace, then merge it on a
working branch. Do not reset to upstream or replace whole editor files when
resolving conflicts: preserve both upstream changes and the Vim integration.

```sh
git status --short
git branch backup/before-upstream-sync
git switch -c sync/upstream-release
git fetch --no-tags https://github.com/texstudio-org/texstudio.git \
  refs/tags/4.9.8:refs/tags/upstream/4.9.8
git merge --no-ff upstream/4.9.8
```

Use the same procedure for the next release. Build with debug tests enabled and
run `QT_QPA_PLATFORM=offscreen ./build/texstudio --auto-tests` before tagging.
Also run `QT_QPA_PLATFORM=offscreen ./build/texstudio --auto-tests --vim-tests`.
This dedicated run enables the Vim UI tests skipped by the quick suite and
isolates them from other tests that alter shared editor state. It checks Vim
modes, editing, visual blocks, marks, substitution, Ex commands, insert-mode
completion, and fork version parsing. Check both logs as well as exit status. Review the merged diff against the upstream tag so it contains
only intentional fork differences.

## Release naming and publishing

Create each fork tag on its own tested merge commit, never on the unmodified
upstream commit. Use `texstudio-vim-<upstream-version>-r<fork-revision>` for tags and downloadable
files. The numeric version inside the application follows upstream; the Git
revision shown in About and `--version` includes the full fork release name.

`r0` is the current Vim baseline. Increment the number in `VIM_REVISION` for
released fork changes on the same upstream base (including Vim and packaging
fixes). Reset it to `0` when adopting a new upstream version. Keep the upstream
beta/alpha/rc designation, for example `texstudio-vim-4.9.9beta2-r1`. The fork
revision is separate from upstream's beta number and Git commits since the tag.

The original `texstudio-vim-4.9.8` and `texstudio-vim-4.9.9beta2` releases remain
available as compatibility aliases for the r0 baseline. Their tags and downloads
are unchanged. The r0 releases rebuild that same Vim baseline with revision-aware
release metadata and filenames.

Push only the intended branch and fork tags; never use `git push --tags` (which
would also publish imported upstream tags), force-push, or move an existing release
tag. After checking the current remote branch, a fast-forward push can publish
the tested commits and these two tags:

```sh
git push --atomic origin HEAD:master \
  refs/tags/texstudio-vim-4.9.8-r0 \
  refs/tags/texstudio-vim-4.9.9beta2-r0
```

CD builds Windows x86_64 and ARM64 installers/portable archives, Linux AppImage,
and Intel/Apple Silicon macOS archives. Release publication waits for the reusable
CI test workflow and every platform build. Only `texstudio-vim-*` tags publish;
beta/alpha/rc tags are marked prereleases. Windows installers are included even
when signing credentials are unavailable. The executable and internal application
bundle names retain the names expected by deployment tools.

Do not substitute upstream release binaries: they do not include Vim support.
