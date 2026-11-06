# Fork changes

Vim editing revisions are recorded in [VIM_CHANGELOG.md](VIM_CHANGELOG.md).

## Unreleased

- Fix remote worker startup in standalone Linux AppImages: use the bundle's
  AppRun launcher rather than treating its ELF loader as the TeXstudio program.
- Add browsing for custom SSH config files, home-directory path expansion and
  separate caches when switching config files.

- Move Apple Silicon CI and package GUI checks to macOS 15 after GitHub retired
  its macOS 14 runners on November 2, 2026.

- Optional SSH remote workspaces for Windows, macOS and Linux: cached project
  editing, background saves, remote TeX compilation, local PDF preview and SyncTeX.
- Revision-checked saves, separately staged downloads and explicit conflict
  comparison/merged upload preserve concurrent local and remote edits.
- Desktop and transport tests for the remote workflow run alongside Vim tests.

This feature is being reviewed; no release or Vim version increment is made.
Setup and current limits are in [REMOTE_WORKSPACE.md](REMOTE_WORKSPACE.md).
