# Review upstream synchronization

For upstream-sync pull requests, independently review the combined behavior,
even when Git reports no conflicts. The editing agent's success and green tests
are not evidence that every upstream interaction is correct.

Inspect upstream changes together with the fork integration, especially:
- Keyboard and mouse event routing, shortcuts, Ctrl-click navigation, and IME.
- Normal, Insert, Replace, and all Visual modes; pending operators and counts.
- Document/cursor lifetime, motions, registers, clipboard, repeat, and undo.
- Completion/snippets, search/substitution, saved settings, and fresh Vim defaults.
- Fork version parsing, update channel selection, branding, and download links.
- Conflict resolutions that drop upstream behavior or fork behavior, even if
  the edited file compiles; identify interactions outside current test coverage.

Read VIM_MODE.md and VIM_TESTING.md. Report concrete bugs, uncovered risks, and
the validation needed. Do not approve solely because tests pass. Do not alter
tests or release controls to make a sync pass. Only approve the exact reviewed
commit when it is ready; unresolved findings must block promotion.
