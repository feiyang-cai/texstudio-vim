# SSH remote workspaces (unreleased)

Edit a paper locally while its project and TeX installation live on another
computer. The module uses system OpenSSH and a private project cache. It does
not require a filesystem mount, SSHFS, a Vim plugin, or a persistent remote agent.
Windows, macOS and Linux are supported as clients and remote hosts.

This feature is under review. It has not been published in a release.

## Prepare the computers

The client needs OpenSSH (`ssh` on PATH). The remote computer needs an SSH server,
Python 3.8 or newer, a TeX installation, and permission to read/write the paper's
folder. Install additional TeX packages and tools on the remote computer.

| Remote system | Typical setup |
| --- | --- |
| Linux | Install OpenSSH server, Python 3 and TeX Live through your distribution. Enable its SSH service. |
| macOS | Enable **System Settings → General → Sharing → Remote Login**. Install Python 3 and MacTeX/BasicTeX. |
| Windows | Install **OpenSSH Server** in Optional Features, start `sshd`, and allow its port in Windows Firewall. Install Python 3 and MiKTeX or TeX Live. |

Follow your organization's access rules. A machine behind a firewall may require
its VPN or an SSH jump host. Existing OpenSSH `ProxyJump`, ports and key settings
work through an SSH config alias. This feature does not create public access to
your machine.

First establish key or SSH-agent authentication in a terminal and verify the
server's host-key fingerprint through a trusted source. For example, add this
to `~/.ssh/config` (Windows: `%USERPROFILE%\.ssh\config`):

```sshconfig
Host office-paper
    HostName office.example.org
    User your-login
    IdentityFile ~/.ssh/id_ed25519
    # Port 2222
    # ProxyJump university-gateway
```

Confirm `ssh office-paper` works. Unlock encrypted keys in your SSH agent before
opening TeXstudio. Password prompts, interactive MFA and first-use host-key
prompts are not supported inside TeXstudio. Passwords and private keys are not
stored in workspace profiles.

## Open and use a project

1. Choose **File → Remote Workspace → Connect over SSH…**.
2. Enter the SSH alias and the remote project's absolute folder, for example
   `/home/me/papers/article`, `/Users/me/papers/article` or
   `C:/Users/me/papers/article`. Select `python3`, `python`, or `py -3` to match
   the remote installation. You can specify a separate OpenSSH config file.
3. Open a `.tex` file by double-clicking it in the remote project panel. Use the
   normal editor and Vim commands. Saving queues an upload in the background.
4. Compile normally. External build commands for this workspace run remotely;
   the internal PDF viewer, logs and SyncTeX navigation use downloaded outputs.
   Standard TeX tools are located on the remote machine even when the client's
   command settings contain a different operating system's executable path.

Wait for **Saved changes synchronized** before relying on a remote save. A local
save is retained even if SSH fails. Compilation uploads saved project files
first; normal TeXstudio rules still control whether unsaved buffers are saved
before a build. Commands are executed with arguments, without inserting file
names into a remote shell command.
The selected folder limits synchronization; build commands run with your SSH
account's normal permissions.

Use **Refresh remote project** after changes made on the remote computer. There
is no continuous remote filesystem watcher. **Synchronize saved changes** retries
an upload after reconnecting. Saved connections are available in the connection
dialog. Normal local projects retain their existing editor and build behavior.

Build outputs and dependencies must stay inside the selected project. Absolute
paths outside it, custom local executables, external include folders, shell
pipelines and interactive terminal programs may need project-specific changes.
TeX search paths under the project are translated for the remote OS. External
PDF viewers remain local. TeX Live, MiKTeX, MacTeX and common TinyTeX locations
are detected; other installations should be on the SSH session's PATH.
The `.texstudio-remote-preview` directory is reserved for generated previews.
Its tracked files are removed remotely when TeXstudio cleans their local copies.

## Conflicts and recovery

Uploads check the previously observed SHA-256 revision and replace files using
an adjacent temporary file. If the remote revision changed, uploading stops.
Downloads are staged separately, then applied on the GUI thread only when the
cached revision and open editor buffer allow it. A transfer cannot silently
replace a dirty buffer or a newer local save.

1. Use **Refresh remote project**, then **Open a downloaded conflict copy…**.
2. Compare that read-only copy with your local file; merge and save locally.
3. Choose **Upload a merged local version…** and confirm the file. This replaces
   only the remote revision you compared. A newer remote change blocks it again.

Automatic merging is not attempted. Multiple simultaneous writers can still
race in external tools that do not participate in revision checks; coordinate
editing with colleagues and keep normal backups.

The private cache lives under Qt's application-data folder in
`remote-workspaces/<connection-id>/project`. The panel's files are ordinary local
copies, so offline edits remain recoverable. Conflict copies are retained outside
that project folder. Closing with unconfirmed uploads asks whether to keep those
cached changes. Keep the cache until you have verified your remote files.

Remote deletions do not silently delete cached files. **Delete remote file…**
checks the remote revision and retains the cache as a recovery copy; editing and
saving it again can recreate the remote file. Renaming files and directories is
not yet a dedicated operation. A project is limited to 1 GiB, 100,000 files and
64 MiB per file. Names must be portable across the three systems; paths escaping
the project, case collisions, external symlinks and directory-link loops are
rejected. The entire selected folder is cached, including hidden files; choose
a project folder rather than your home directory.

## Maintenance and validation

The transport, transient Python helper and project controller are isolated in
`src/remoteworkspace/`. The editor and Vim engine operate on ordinary cached files.
Integration points are the File menu, file-open/save notifications, external
build dispatch, preview temp directory and SyncTeX source-path resolution.
No Vim bindings or revision numbers change for this feature.

Run helper and standalone Qt tests:

```sh
python3 tests/remote/test_agent.py
cmake -S tests/remote -B build-remote-tests
cmake --build build-remote-tests
python3 tests/remote/run_ssh_tests.py build-remote-tests/remote-workspace-tests
```

Use `.exe` on Windows. OpenSSH server must be installed for the disposable
loopback fixture. It creates temporary test keys, pins its own host key, listens
only on loopback, and leaves the user's SSH configuration untouched.

With a Debug application built with `TEXSTUDIO_ENABLE_TESTS=ON` and the internal
PDF viewer, run `python3 tests/remote/run_gui_tests.py <texstudio-executable>` on a
desktop (or Linux Xvfb). The test exercises native Vim editing, background save,
real SSH LaTeX compilation, PDF rendering and both directions of SyncTeX.
Logs and screenshots go to `remote-test-results/`. Existing Vim suites run
separately. CI exercises Linux, macOS Intel/ARM and Windows x64/ARM; consult the
actual check results rather than treating a configured job as a passing test.
