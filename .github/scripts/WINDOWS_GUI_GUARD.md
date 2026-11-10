# Hosted Windows GUI guard

This is CI harness code, not application behavior or an operating-system fix.
The packaged editor assertions, package commit identity, nine packaged desktop
jobs, four native platforms, and release gates are unchanged.

## Scope

The process guard runs only with explicit `TEXSTUDIO_HOSTED_WSL_GUARD=1` plus
GitHub-hosted Windows ARM runner environment checks. Both packaged workflows
set the opt-in only for `windows-11-arm`. Do not run it on a user desktop.

It may terminate only a process meeting **all** these conditions:

- Its reported executable is the native `%SystemRoot%\System32\wsl.exe`.
- Windows command-line parsing produces that same absolute executable or the
  bare name `wsl.exe`, followed by exactly `--update --confirm --prompt-before-exit`, in that order.
- It belongs to the test's interactive session.
- A held process handle confirms the same executable, creation timestamp at
  CIM's microsecond precision, session, and a still-running process immediately
  before termination. That same handle is used for termination and completion.

Missing identity, PID reuse, denied access/cleanup, observer failure, stale
heartbeat, and shutdown timeout fail the job. Ordinary WSL commands and
WindowsTerminal are never termination targets. Short-lived process-start events
are diagnostic only because names cannot establish an authorized target.
Ancestry PID/path/session/creation metadata is diagnostic only. It is **not** a
verified provisioner allowlist; ancestor command lines are never logged.

There are no new registry, UAC, IFEO, task-scheduler, executable replacement,
installation, or security-setting changes. The existing
`prepare-windows-gui.ps1` is unchanged and still has its prior first-login OOBE
registry/process preparation. This patch must not be described as removing all
existing desktop setup changes.

## Input correctness

Startup recovery may activate the app or click its caption only after a native
hit test identifies the app as the caption owner. No keys or title-based window
close requests go to an unknown foreground window.

Before editing, a native probe tests the actual foreground hook. It pauses only
the observer's message pump, shows an owned child-process obstruction, destroys
that window, restores the test window, then fences the WinEvent stream. It must
observe the known obstruction and retain the focus-loss failure after return.
A monitor/timeout failure cannot count as a successful probe.

After arming, every keyboard/mouse wrapper and every `SendInput` call checks
focus. An out-of-context WinEvent hook has its own message pump and preserves
all foreign/unknown foreground transitions, including destroyed windows and
away-and-back transitions. No restoration, replay, or fault reset is permitted.
Unique serialized fences use actual hidden-listbox state changes and matching
WinEvent object/child identifiers, not ordinary posted-message acknowledgments.
The observer is stopped and checked, then the foreground stream is fenced and
unhooked before a passing result is written.

## Limits and validation

Process detection/termination is best effort. The updater can open a window
before observation or reject termination. Cleanup uses exit code 1; depending
on the runner Terminal profile, a Terminal tab may survive that exit. It must
remain a blocker, never trigger broader cleanup. Neither a quiet startup nor a passing
unit suite proves that it will be prevented. Any resulting focus interference
must fail rather than be hidden by cleanup. Native event ordering, PowerShell
CIM behavior, GUI readiness, and the mitigation's effectiveness require the
actual hosted Windows run; Linux mocked tests cannot validate those properties.

Local tests:

    python -m unittest discover -s .github/scripts/tests -p 'test_*.py'

Before publishing/running this branch, obtain approval for the exact process
boundary above and the external GitHub actions. One original-artifact retest can
use `packaged-gui.yml` with `build_run_id=42832542074`; it must preserve immutable
source `066c2c8c09e9995129efdc903d3480a8e9c72255`. Do not move the alpha tag or
publish a release until all required checks pass. This local patch alone is not
release approval or proof of readiness.
