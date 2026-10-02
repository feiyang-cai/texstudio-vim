# Vim development test build

This branch is a development snapshot based on `texstudio-vim-4.9.9beta2-r0`.
`VIM_REVISION` remains `0`. It creates no release tags or GitHub releases; a fork
revision will be assigned only after the maintainer approves publication.

## Linux x86_64 testing

Download the `texstudio-linux` artifact from this branch's **CD** workflow run,
unzip it, and make the AppImage executable:

```sh
chmod +x texstudio-vim-linux-*.AppImage
mkdir -p "$HOME/.config/texstudio-vim-dev"
./texstudio-vim-linux-*.AppImage --start-always --config "$HOME/.config/texstudio-vim-dev"
```

The separate configuration directory lets you test without changing your usual
TeXstudio settings. This AppImage is built on Ubuntu 24.04. If FUSE is unavailable,
run it with `--appimage-extract-and-run` before the application arguments.

Enable **Options > Configure TeXstudio > Editor > Editing Mode > Vim
(experimental)**. About and `--version` identify the baseline plus the development
commit, rather than a new release number.

## What to try

| Keys in Normal mode | Expected behavior |
| --- | --- |
| `"ayy` then `"ap` | Yank a line into register a, then paste it |
| `"Ayy` | Append another line to register a |
| `yy`, delete something, then `"0p` | Paste the most recent unnamed yank |
| `dd`, `dd`, then `"2P` | Paste the earlier line deletion |
| `x` then `"-p` | Paste the small deletion |
| `"_dd` | Delete without changing registers |
| `"+yy` / `"+p` | Copy to / paste from the system clipboard |
| `"a3p` | Paste register a three times, with one undo transaction |
| Visual selection followed by `"ap` | Replace selection using a; preserve a |
| `"add` followed by `.` | Repeat the line deletion using a |

Named registers are shared between open documents for this application session.
They are not saved when the application exits. Uppercase register names append
to their lowercase counterpart. The `*` register uses primary selection where
available and otherwise uses the ordinary clipboard. Special Vim registers such
as expression (`=`), search (`/`) and filename (`%`) are not implemented yet.

Also try the existing completion, snippets, search, marks, visual block operations
and insert/replace modes. Report the command sequence, original text, expected
result, actual result, OS and the development commit from About.

## Module structure and validation

- `src/viminputbinding.h`: narrow input-binding and prompt interfaces used by
  TeXstudio, with factory functions that hide the implementation.
- `src/viminputbinding.cpp`: modal state, command dispatch, motions, editing and
  prompt handling; delegates Insert mode to TeXstudio's default binding.
- `src/vimregisters.h/.cpp`: typed session registers and clipboard exchange,
  independent of editor/document ownership.
- `src/latexeditorview.cpp`: TeXstudio integration, completion, search and Ex
  execution. These remain the adapter surface for future upstream merges.

The expanded Vim tests cover named registers, appending, automatic history,
clipboard exchange, sharing between views, visual paste, counts and undo. Existing
Vim/completion/version coverage stays enabled. Configure a Debug build with
`TEXSTUDIO_ENABLE_TESTS=ON` and run:

```sh
QT_QPA_PLATFORM=offscreen ./texstudio --start-always --auto-tests
QT_QPA_PLATFORM=offscreen ./texstudio --start-always --auto-tests --vim-tests
```

This milestone does not add recorded Vim macros, mappings, Vimscript or complete
Vim compatibility. Those remain later development work.

## Cross-platform desktop checks

The **Vim desktop tests** workflow builds Debug test binaries and runs the Vim,
completion and version suites on Windows x64, Windows ARM64, macOS Intel and macOS
Apple Silicon hosted VMs. It uses the native Qt desktop platform (Windows or Cocoa).
The existing Linux **CI** workflow also runs the suite under Xvfb with the X11
platform, in addition to its offscreen checks. Each desktop run uploads its log
and a screenshot of the Unicode clipboard test as a `vim-tests-*` artifact.

The **CD** workflow runs the exact packaged AppImage in Ubuntu 22.04/24.04, Debian
12/13 and Fedora 43 containers on hosted Linux VMs. Each container has an X11
session. Its smoke test checks the package's commit hash, opens a fixture, sends
named-register yank/delete/paste commands and uppercase insertion/dot repetition
through the window, saves the result, and uploads logs plus a desktop screenshot as an `appimage-smoke-*` artifact.
These are distribution compatibility checks, not separate desktop VMs per distro.

The test launcher rejects failed, skipped or missing suites and enforces a timeout.
A successful build alone is not counted as a successful desktop test. The new
clipboard coverage includes Unicode, external CRLF text and the primary-selection
register (or its clipboard fallback on platforms without primary selection).

These checks exercise the test fixtures and packaged editing path; they do not
establish compatibility with every window manager, display server or keyboard
layout. Native Wayland and physical-machine testing remain separate follow-ups.
