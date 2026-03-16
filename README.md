# texstudio-vim

This is an experimental fork of [TeXstudio](https://github.com/texstudio-org/texstudio) with native Vim-style editing support.

This is not an official TeXstudio release, and it should be treated as unstable. Some Vim behavior is implemented, some is still incomplete, and there will be bugs.

## Important warning

This fork is largely AI-generated.

What I mainly did was:

- prompt
- guide direction
- test behavior

I did not manually design and review every code path to production standard. Use this carefully, especially for important work.

## Current status

- Experimental Vim support is the main goal of this fork.
- Supported Vim behavior is documented in [VIM_MODE.md](VIM_MODE.md).
- This is not full Vim parity.
- Expect edge-case bugs and regressions.

## Downloads

- Releases: [GitHub Releases](https://github.com/feiyang-cai/texstudio-vim/releases)
- Current planned prerelease tag: `v4.9.3beta1-vim-preview1`
- Release artifacts for supported platforms are intended to be distributed from GitHub releases for this fork, including macOS, Linux, and Windows.

## Feedback

Feel free to test it and send feedback, especially if you find:

- broken Vim motions or operators
- insert-mode regressions
- search and replace issues
- visual mode or block mode bugs
- crashes or data-loss risks

## Upstream

The base editor project is TeXstudio:

- upstream repo: [texstudio-org/texstudio](https://github.com/texstudio-org/texstudio)
- official website: [texstudio.org](https://www.texstudio.org)
