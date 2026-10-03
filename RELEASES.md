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

## Daily upstream synchronization

The `Upstream release sync` workflow checks stable and prerelease releases daily
and can be run manually from the default branch. It starts each candidate from
the matching stream's latest published fork commit, never from the moving
`master` tip, and requires the candidate to contain only one merge of that base
and the pinned upstream commit. It verifies that the published base and candidate
both retain the approved `VIM_REVISION`. Sync PRs cannot change workflow,
action, script, test, package
identity, or publication controls.

After merge, the trusted controller stages the tested application tree with the
current base-branch `.github` tree, then reruns CD and Vim-platform checks on that
exact release candidate. This ensures the tag's CD release job enforces the
publication switch even though release source commits are based on earlier
published tags.

Dry-run procedure: run **Actions → Upstream release sync → Run workflow** on the
default branch. `.github/upstream-release-policy.json` defaults to
`"dry_run": true` and `"publication_enabled": false`; the run reports releases
and planned issue/controller actions without creating issues, merging PRs,
tagging, or publishing. To permit sync issue creation and assignment, an owner
must change `dry_run` to `false` in a reviewed default-branch change. Automatic
merge and release tagging additionally require an owner-reviewed change setting
`publication_enabled` to `true` and the repository Actions variable
`UPSTREAM_RELEASE_PUBLICATION_ENABLED=true`. The Actions variable is also checked
by the current CD release job. Either switch being disabled keeps automatic
release mutations off. Create release tags only through the trusted controller;
older immutable CD workflow revisions predate this guard.

Configure
`COPILOT_SYNC_TOKEN` as a user-to-server token that can create issues and assign
Copilot; it is exposed only to the trusted detector step. Each issue pins the
upstream release tag and resolved commit SHA. The trusted controller reuses
pending issues/PRs and requires the `CD` and `Vim desktop tests` workflows to
pass on the exact sync head before merging. It then dispatches both workflows
on the merge commit, creates an immutable fork tag only after those checks pass,
and dispatches `CD` on that tag. Publication waits for all platform builds and
all nine packaged GUI checks. Stable and prerelease progress is recorded
separately only after the matching fork release is published.

GitHub-required approval of Copilot workflow runs is not bypassed. Syncs preserve
the approved Vim baseline and `VIM_REVISION`; changing either requires an
owner-approved change.

### Independent AI review

Upstream syncs additionally require an independent **Copilot code review**,
separate from the Copilot coding agent that prepares the merge. The controller
requires the review bot's latest review of the exact PR head to be `APPROVED`,
and every Copilot review thread must be resolved. Missing, comment-only,
changes-requested, dismissed, and stale reviews block merging and release
tagging. A new commit needs a new review. GitHub API failures also block promotion.
The review instructions focus on behavioral upstream/Vim interactions, including
changes that Git merges without conflicts and gaps not covered by current tests.

The trusted `Upstream AI review` workflow requests reviews when eligible sync PRs
are ready or updated, without checking out candidate code. Daily/manual sync
runs retry missing review requests and recheck reviewed PRs, so a review that
finishes after CI can be picked up on the next daily check or manual run.
Dry-run mode sends no review requests. Publication switches remain independent
and disabled by default; AI approval alone cannot publish a release.

Configure **Settings → Copilot → Code review → Auto-approval → Allow Copilot to
approve pull requests** in this repository. Copilot otherwise normally submits
comment-only reviews, which deliberately do not satisfy this gate. If the
approval feature is unavailable for the account, automatic promotion remains
blocked; a comment saying the code looks good is never treated as approval.
Copilot review uses the existing `COPILOT_SYNC_TOKEN` user credential and
subscription. Review findings need fixes and a new review; tests still remain
required. AI review reduces risk but does not guarantee correctness.
If Copilot already submitted a comment-only review before auto-approval was
enabled, manually request another review on that PR. The workflow avoids
repeated requests for a commit that Copilot has already reviewed.

## Release naming and publishing

Create each fork tag on its own tested merge commit, never on the unmodified
upstream commit. Use `texstudio-vim-<upstream-version>-r<fork-revision>` for tags and downloadable
files. The numeric version inside the application follows upstream; the Git
revision shown in About and `--version` includes the full fork release name.

`r1` is the approved Vim baseline, recorded with its commit and branch in
`.github/approved-vim-baseline.json`. Preserve `VIM_REVISION` when adopting a
new upstream version; an upstream update alone does not authorize a revision
change. Automatic sync releases use the approved Vim baseline and must not
include unapproved Vim development from `master`. Advance the approved baseline
only through owner-approved changes. Keep the upstream
beta/alpha/rc designation, for example `texstudio-vim-4.9.9beta2-r1`. The fork
revision is separate from upstream's beta number and Git commits since the tag.

The legacy unnumbered `texstudio-vim-4.9.8` and `texstudio-vim-4.9.9beta2` tags
remain as compatibility aliases for the r0 baseline. Existing tags are not moved.
A tag alone does not establish that a downloadable GitHub release has been published.

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
CI test workflow, every platform build, and all nine packaged GUI checks. Only `texstudio-vim-*` tags publish;
beta/alpha/rc tags are marked prereleases. Windows installers are included even
when signing credentials are unavailable. The executable and internal application
bundle names retain the names expected by deployment tools.

Do not substitute upstream release binaries: they do not include Vim support.

## 4.9.9beta2-r1

The r1 release uses upstream 4.9.9beta2 and remains a prerelease. It adds typed
shared, named and clipboard registers; expands mode, motion, search and
substitution coverage; fixes cursor boundaries, vertical columns, visual paste,
repeat, multiline/block undo and redo shortcuts; and preserves Ctrl-hover and
Ctrl-click navigation in Normal mode. Vim is the default for fresh configurations,
while saved preferences are preserved. Update notifications use published releases
from this fork and compare the numeric Vim revision; installation remains manual.

See [VIM_TESTING.md](VIM_TESTING.md) for verified behavior and remaining gaps.
Publication is gated on Linux CI and all packaged Windows/macOS/Linux GUI checks.

## Release order

Publish `texstudio-vim-4.9.8-r1` first as the stable upstream-base release, then
`texstudio-vim-4.9.9beta2-r1` as a prerelease. Both carry the same r1 Vim changes
and require their own successful builds and packaged desktop checks. The beta
release must not replace the stable release in GitHub's latest stable channel.

## Recovering publication after a CI-only validation fix

Published tags are never moved. If an existing tag has complete successful
platform builds and regression checks but its publication was blocked by a
CI-only checker issue, `.github/release-request.json` pins the tag, source SHA
and original CD run. The verified-tag publication workflow rejects application
changes, verifies the immutable tag and build provenance, reruns all nine GUI
environments, and uploads the original build payload to a draft. It publishes
only after every expected asset has finished uploading. Existing published
release assets are not replaced.

Exact-tag Windows builds display the canonical fork tag without a commit suffix.
Their GUI checks require both the canonical version and the source commit in
the archive filename from the pinned GitHub build. Branch builds and the other
platforms retain the binary commit-suffix check.
