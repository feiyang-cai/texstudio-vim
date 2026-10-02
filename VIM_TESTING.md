# Vim development verification

The tests below cover the experimental commands documented in `VIM_MODE.md`.
A passing build does not establish complete Vim compatibility or exhaustive
coverage of every combination. No release is authorized by these tests.

## Automated coverage map

| Documented behavior | Behavioral tests | Scope |
| --- | --- | --- |
| Fresh-install Vim default | Packaged desktop smoke scripts, with no `Editor/EditingMode` override | Actual OS input and saved text |
| Switching Vim/standard; cursor styles | `vimEditingModeSwitches`, `vimCursorStyles` | Qt editor integration |
| Insert/append/open lines; Replace overwrite; Escape and Ctrl-[ | `vimDocumentedModes`, `vimInsertEscape`, `vimCloseElementEscapesInsertMode` | Text, mode, cursor assertions |
| Character/line visual modes | `vimDocumentedModes`, visual register rows | Text and register assertions |
| Block mode, delete, left-edge insertion | `vimVisualBlockCtrlV`, `vimVisualBlockDeleteAffectsAllRows`, `vimVisualBlockInsertAtStart` | Shortcut and multi-row editing assertions |
| h/j/k/l, counts, boundaries; w/b/e; 0/^/$; gg/G | `vimDocumentedMotions` | Exact cursor positions and unchanged text |
| f/F/t/T, repeat ; and reverse , | `vimDocumentedMotions`, register rows | Exact positions, counts, missing-character behavior |
| d/c/y, line edits, single-character replacement, inclusive/exclusive ranges | `vimDocumentedModes`, `vimRegisterCommands` | Text, mode, register payloads |
| Registers, p/P, counts, visual paste, dot repeat, undo | `vimRegisterCommands`, `vimRegisterStore`, `vimPhysicalModifierEvents` | Typed payloads and document assertions |
| Cross-editor and clipboard registers | `vimRegistersSharedAcrossViews`, `vimClipboardRegisters`, `vimDesktopClipboard` | Shared state, Unicode, clipboard MIME and line endings |
| / and ?, n and N, cancelling a search | `vimSearchNavigation` | Prompt input and resulting line positions |
| Prompt submission and history | `vimPromptEnterDoesNotInsertNewline`, `vimPromptHistory`, `vimSubstitutePrompt` | Keyboard path, error correction, command/search history |
| Substitution: current line, %, numeric range, g/i/I, delimiter, regex, & | `vimSubstituteFlags`, `vimExSubstituteCommands` | Document results and rejection without mutation |
| Substitution: c flag | `vimSubstituteConfirmation` | Actual dialog Yes/No decisions and one-step undo |
| Substitution: remembered pattern, undo/redo across lines | `vimExSubstituteCommands`, `vimSubstitutePrompt` | Reuse and complete document restoration |
| Marks, exact and line jumps, operator marks | `vimMarks` | Cursor and editing assertions |
| Ex write/quit dispatch, numeric line command | `vimExCommands` | Command signals and cursor assertions |
| Insert-mode completion | Focused `LatexCompleterTest` cases | Editor/completion integration |

Coverage still needs expansion for paragraph/pair motions, whole-word */#
semantics, search wrapping/counts, nested/delimiter-specific text objects,
indent/join combinations, block yank/change/paste on uneven lines, all Ex aliases,
confirmation cancellation, and snippet/auto-pair/macro/link combinations. These
are not claimed as verified merely because they appear in the documentation.

## Desktop environments

The Debug suite runs on Linux, Windows x64/ARM64 and macOS Intel/Apple Silicon.
Qt-generated input exercises editor behavior in those jobs.

The CD workflow additionally tests the shipped Linux AppImage under Xvfb/Openbox
on Ubuntu 22.04/24.04, Debian 12/13 and Fedora 43. Native Windows/macOS jobs unpack
the portable archive or application bundle and drive a visible window using
PyAutoGUI and host OS input APIs. Every packaged test uses a fresh configuration,
checks the commit in the executable version, and verifies saved text after named
registers, Insert/dot, Replace, character/line/block Visual editing and undo,
global substitution with undo/redo, and forward/backward searches.
Screenshots, application logs and intermediate saved results are retained.

`Packaged desktop retest` can reuse an existing CD build when application sources
are unchanged, allowing CI-only desktop fixes to be checked without recompiling on all nine
packaged desktop environments.
It verifies that subsequent changes affect only CI/documentation and records the
package commit. Windows runner setup disables the first-login privacy wizard on
the disposable CI VM so it cannot steal keyboard focus.

The Windows/macOS packaged jobs require a successful hosted
run before their behavior can be called verified. Missing interactive desktop,
Accessibility or screen-recording permission is reported as a failure/blocker,
not a passing test. These virtual runner checks do not cover physical keyboards,
IME composition, Wayland, every display scaling setting, or TeX compilation.

## Local focused run

Build with `TEXSTUDIO_ENABLE_TESTS=ON` in Debug mode, then run:

```sh
QT_QPA_PLATFORM=offscreen python3 .github/scripts/run-vim-tests.py /path/to/texstudio
```

The runner rejects failures/skips and saves the report and screenshot. Run without
`QT_QPA_PLATFORM=offscreen` to test the native Qt window system. Packaged smoke
scripts require their respective desktop and packaging environments.
