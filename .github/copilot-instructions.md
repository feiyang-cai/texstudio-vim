# Review upstream synchronization

For upstream-sync pull requests, independently review the combined behavior,
even when Git reports no conflicts. The editing agent's success and green tests
are not evidence that every upstream interaction is correct.

Preserve upstream behavior; allow only the changes needed for Vim compatibility.
If upstream behavior needs changing, or preservation is uncertain, require human
validation. Ask for the `upstream-behavior:human-validation-required` declaration
in the PR and explain the proposed change, necessity, and behavioral impact.
All manual conflict resolutions and edits beyond the automatic pinned merge
require human validation. Do not remove an escalation or treat AI approval as
human approval. Independent AI review and tests remain required as well.

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
