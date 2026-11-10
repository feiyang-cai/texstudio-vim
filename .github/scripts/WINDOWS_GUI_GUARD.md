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

The process guard itself makes no registry, UAC, IFEO, task-scheduler,
executable replacement, installation, or security-setting changes. The separate
manual IFEO option below requires explicit security-setting approval. The existing
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

## Optional one-run IFEO workaround (approval required)

This option is **off by default** and is not enabled in ordinary push, PR, CD,
or reusable runs. Before dispatch, obtain action-time approval for the exact
workflow commit and the following operation. Select `temporary_wsl_ifeo=true`,
set `wsl_ifeo_approved_head` to that exact commit, and use original build run
`42832542074`. Only attempt 1 is permitted; rerunning a job is not covered.
The journal binds the repository, workflow head, run ID, attempt and original
build identity. The manual switch is an execution gate, not proof of consent;
the operator must obtain approval before selecting it. Its distinct concurrency
group prevents an ordinary branch run from canceling this temporary operation.

On that disposable GitHub-hosted Windows ARM runner only, before package setup:

1. Require the native IFEO parent to resolve to the inspected canonical key and
   require the entire `wsl.exe` subkey to be absent, even if an existing key has
   no Debugger. A native registry transaction creates only that direct subkey
   and its REG_SZ `Debugger`, pointing to the quoted, existing built-in native
   `System32\systray.exe`. No second logical or literal WOW key is written.
2. This temporarily redirects **all WSL launches**, including legitimate WSL
   use and runner provisioning, not just the updater. It prevents new launches
   only. An already-open prompt or any foreground interference still fails the
   strict GUI checks. No Terminal window is closed or its focus loss hidden.
3. An `always()` cleanup step uses a fresh native registry transaction. It
   checks the exact Debugger bytes, key value/subkey shape, canonical identity,
   class size, security-descriptor size and last-write timestamp against the
   committed setup snapshot. Every observed drift or transaction error fails;
   there is no overwrite, retry or nontransactional fallback. It removes only
   that Debugger value and deliberately **leaves an empty key shell**. It never
   deletes the key or changes ACLs, other settings, or other executable entries.

This is an **exact observed-state contract, not proof of ownership history**.
An identical external rewrite between transactions cannot reliably be
identified; timestamps are not unique revision tokens. The bounded operation
assumes an isolated disposable runner and does not claim to detect every
historical write. Transactions protect the individual compare/change operation.
No marker registry value, ACL change, or broader ownership mechanism is added.

If setup commits but the process/journal update is interrupted, cleanup refuses
to infer ownership from a matching value. Failed or uncertain cleanup fails the
job and may leave the redirect until that disposable VM is retired. `always()`
cannot guarantee execution after machine loss, forced cancellation or job timeout.
Never reuse that runner or claim successful restoration in those cases.

The read-only native inspection on run `42852256989` found all four target keys
absent and all four logical/literal parent handles aliased to the same canonical
IFEO parent. It verified native systray availability, **not** transaction support
or mitigation effectiveness. Local tests use fake APIs. The first explicitly
approved native run must validate the actual transaction setup/cleanup and all
nine original-package GUI jobs; unsupported APIs or conflicts fail closed. The
existing foreground probes, final fences, editor assertions and release gates
remain unchanged. An opt-in or mocked pass is never release approval.
