# Vim Mode Support

This branch adds an experimental Vim editing mode for TeXstudio.

## Default and configuration

Vim is the default editing mode for fresh configurations in texstudio-vim.
Existing saved editing-mode preferences are preserved. To change the mode, use
`Options > Configure TeXstudio > Editor > Editing Mode`.

## Supported Modes

- `NORMAL`
- `INSERT`
- `REPLACE`
- `VISUAL`
- `V-LINE`
- `V-BLOCK`
- command prompt `:`
- forward search `/`
- backward search `?`

## Supported Motions

- `h`, `j`, `k`, `l`
- `w`, `b`, `e`
- `0`, `^`, `$`
- `gg`, `G`
- `{`, `}`
- `%`
- `f`, `F`, `t`, `T`
- `;`, `,`
- `*`, `#`
- counts on motions and operators

## Supported Editing Commands

- mode changes: `i`, `a`, `I`, `A`, `o`, `O`, `R`, `Esc`, `Ctrl-[`
- deletes/changes/yanks: `d`, `c`, `y`, `dd`, `cc`, `yy`, `D`, `C`, `Y`
- character and line edits: `x`, `X`, `s`, `S`, `r`, `J`
- indenting: `>>`, `<<`
- paste: `p`, `P`
- undo/redo: `u`, `Ctrl-r`
- repeat: `.`

## Registers (development branch)

- unnamed: `"`
- named: `a`–`z`; uppercase names append to lowercase registers
- yank history: `0`
- line/multiline delete history: `1`–`9`
- small deletions: `-`
- black hole: `_`
- system clipboard: `+`; primary selection (where supported): `*`
- register prefixes in Normal and Visual modes, e.g. `"ayy`, `"ap`, `"_dd`
- characterwise, linewise and blockwise payloads shared between documents

Explicit register destinations preserve automatic yank/delete history. Registers
live for the current application session. Development build instructions and test
examples are in [VIM_DEVELOPMENT.md](VIM_DEVELOPMENT.md).

## Visual Modes

- characterwise visual: `v`
- linewise visual: `V`
- blockwise visual: platform visual-block shortcut
  - most platforms: `Ctrl+V`
  - current macOS Qt build tested here: `Cmd+V`
- blockwise delete/yank/change/paste
- block insert at left edge with `V-BLOCK`, `Shift+I`, type, `Esc`

## Text Objects

- `iw`, `aw`
- `i(`, `a(`
- `i[`, `a[`
- `i{`, `a{`
- `i"`, `a"`
- `i'`, `a'`

## Search

- `/pattern`
- `?pattern`
- `n`, `N`
- `*`, `#`
- `:noh`, `:nohlsearch`
- prompt history for `:`, `/`, `?`

## Ex Commands

- `:w`
- `:q`
- `:wq`
- `:x`
- `:write`
- `:quit`
- `:<line>`
- `:s/pat/repl/`
- `:s/pat/repl/g`
- `:%s/pat/repl/g`
- `:1,3s/pat/repl/`
- substitute flags: `g`, `c`, `i`, `I`
- `:s//repl/` reuses the last search pattern

Note: substitute uses TeXstudio's search engine, so regex behavior is Qt-style regex behavior rather than full native Vim regex semantics.

## Marks

- set local marks with `m{letter}`
- jump to mark line with `'{letter}`
- jump to exact mark position with `` `{letter}``
- return to previous jump with `''` and `` ` ``
- operator-pending mark motions such as `d'a`, `c'a`, `y'a`, `` d`a ``

## Insert-Mode Integration

The Vim wrapper keeps TeXstudio's insert-mode features active:

- LaTeX command completion
- snippet placeholders
- auto pairs
- macro expansion
- Ctrl-click style links

Ctrl-hover highlighting and Ctrl-click navigation also work in Normal mode,
without entering Insert mode or changing the document.

## Current Scope / Known Gaps

Not implemented in this branch:

- macro recording/replay
- remapping
- `.vimrc`
- global or file marks
- full jump list
- full blockwise append semantics like Vim's multi-cursor `A`
- full Vim regex and ex command parity

Vim remains experimental, but is the default for fresh texstudio-vim configurations.
Existing saved editing-mode preferences are preserved.

## Development testing

`Vim desktop tests` builds a Debug editor and runs the focused Vim, completion,
and version suites on native Windows x86_64/ARM64 and macOS Intel/Apple Silicon
runners. Linux CI runs the suite both offscreen and in an X11 desktop. Reports
and a Unicode clipboard screenshot are saved as workflow artifacts.

CD additionally exercises the packaged Linux x86_64 AppImage in Ubuntu 22.04/24.04,
Debian 12/13, and Fedora 43 containers on hosted Linux runners. Each check verifies
the commit identity, desktop startup, named registers, Insert/dot, Replace, all
three Visual modes, search, substitution, undo/redo, and saving through keyboard input. These distribution checks use containers, not separate virtual
machines, and do not cover hardware acceleration or a full installed TeX system.

Branch builds are test artifacts. Publishing a release requires explicit approval
and a version tag; passing these checks alone does not publish or bump a version.

## Update notifications

The update checker reads published releases from `feiyang-cai/texstudio-vim`
and links to this fork's release downloads. It ignores drafts, upstream-only tags,
and development commit snapshots. It compares the upstream version and channel
before the numeric Vim revision, so `texstudio-vim-4.9.9beta2-r1` updates r0 of the
same beta. Stable, release-candidate and development preferences still apply.
Checks use the most recent 100 releases returned by GitHub. A release must use
our `texstudio-vim-<upstream-version>-r<revision>` tag format; beta/RC releases
should be marked as prereleases.

Automatic checking shows a notification and download link. It does not download
or install the new application automatically. Existing installations receive
this fork-specific checker only after installing a build containing this change.
