# Vim Mode Support

This branch adds an experimental Vim editing mode for TeXstudio.

## Enable

`Options > Configure TeXstudio > Editor > Editing Mode > Vim (experimental)`

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

## Current Scope / Known Gaps

Not implemented in this branch:

- named registers
- macro recording/replay
- remapping
- `.vimrc`
- global or file marks
- full jump list
- full blockwise append semantics like Vim's multi-cursor `A`
- full Vim regex and ex command parity

This mode is intended as an opt-in experimental editor mode and does not change default TeXstudio behavior.
