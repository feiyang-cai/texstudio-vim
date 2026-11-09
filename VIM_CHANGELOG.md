# Vim changelog

Each Vim revision records user-visible changes here. Upstream-only syncs keep the
Vim revision unchanged. Release names combine the two versions:
`texstudio-vim-<upstream-version>-r<vim-version>`.

## TeXstudio 4.9.9 with r1.1 — 2026-10-28

Stable upstream base `4.9.9`, retaining Vim revision `r1.1` and its mouse-selection
fixes. The Vim implementation is unchanged; this release also includes the
following fork updater improvements:

- Optional automatic installation for writable Linux x86_64 AppImages. Download
  and verify the complete application, then replace it atomically on normal exit,
  keeping the previous image as a backup. No forced restart or privilege elevation.
- Update selection compares upstream TeXstudio first, then numeric Vim revisions,
  and selects the newest release allowed by the chosen channel.
- Independent update requests keep their own settings; long sessions also check
  the configured interval hourly.

Upgrade once manually to `texstudio-vim-4.9.9-r1.1` to get the AppImage installer.
Automatic installation is off by default. Existing settings are preserved.

## r1.1 — 2026-10-18

Published as `texstudio-vim-4.9.9rc1-r1.1` (upstream prerelease).

Upgrade note: existing `r1` binaries recognize only whole-number Vim revisions,
so install `r1.1` manually from this repository's releases. The new updater
recognizes both old and dotted revisions for subsequent update notifications.

- Dragging text in Normal mode enters characterwise Visual mode and preserves
  the exact selection. Double-click word selection also enters Visual mode.
- Vim motions, yank/delete, Escape and Visual mode switches handle forward and
  backward selections, newline boundaries, folded text and Unicode characters.
- Mouse actions reset pending operators and remembered motion columns. Ctrl-click
  navigation remains available; Insert/Replace keep native selection behavior.
- Context menus preserve the selection for Copy/Cut while open and clear stale
  Normal-mode selections when dismissed. Ctrl-drag/right-click cleanup prevents
  later edits from unexpectedly replacing an old selection.
- Visual indentation excludes a following line touched only at column zero and
  correctly includes the final selected line at the end of a document.
- Version parsing, package names and update comparisons accept numeric Vim patch
  revisions: `r1 < r1.1 < r1.2 < r1.10 < r2`.

## r1 — published baseline

- Expanded native Vim editing, registers and clipboard integration, motions,
  editing, Visual modes, search/substitution, marks, Ex commands, repeat and undo.
- Vim enabled by default for fresh configurations, with Ctrl-click navigation.
- Fork branding and update checks link to this repository's releases.
- Linux, Windows and macOS regression tests and packaged GUI checks.

The implementation remains experimental and incomplete; supported commands and
limitations are documented in [VIM_MODE.md](VIM_MODE.md).
