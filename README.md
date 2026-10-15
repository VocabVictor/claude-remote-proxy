# Claude Remote Proxy

## Existing remote projects (this fork)

For projects already on a company server, set `CLAUDE_PROXY_MODE="remote"`
in your local `.claude-remote-proxy.env`, together with `SSH_TARGET`,
`REMOTE_USER`, and `REMOTE_PROJECT_DIR`. Set `REMOTE_USER` to your SSH login
user when possible; root and `su` are then unnecessary.

Run `claude-remote-proxy start` to create or attach to a persistent remote
Claude Code terminal. This mode never uploads files or local sessions, never
changes workspace ownership, and refuses the `sync` command. A fresh remote
conversation is assigned a stable UUID; later process restarts resume it.
The remote project directory must already exist. Linux, Bash, `flock`, tmux,
and authenticated Claude Code are required on the remote host.

The warm start uses one SSH control check and one interactive SSH channel.
Preparation happens in that channel, avoiding repeated network round trips.
`tmux new-session -A` creates or attaches without a check/create race.
Concurrent starts serialize UUID initialization with `flock`.

The default mode remains `import` for compatibility. SSH disconnection leaves
tmux running; run `start` again to reconnect. This is not automatic reconnect,
Mosh predictive echo, a model API proxy, or a file synchronization service.
Any process-scoped network proxy for Claude must be configured separately on
the remote host. This tool does not change the server's network settings.

Set `REMOTE_CLAUDE_BIN` to an absolute executable path when Claude is not on
the remote login shell's PATH.

### Native fast path (Linux / WSL)

Build with `make -C native`. Install `native/claude-remote-proxy-native` on
both the client and server (build separately for different architectures).
Use `examples/native.env.example` as the client configuration template.
Set `REMOTE_PROXY_BIN` to the absolute server-side binary path, then run
`native/claude-remote-proxy-native start` locally. Claude Code still executes
in the remote project. The native client only supports remote mode.

The C++17 client directly replaces itself with OpenSSH. OpenSSH automatically
reuses or creates its control connection, without a separate connection probe.
The remote C++ helper replaces shell bootstrap commands, hashes paths in-process,
reads an existing UUID without locking or writing, and directly executes tmux.
There is no Python runtime, custom forwarding loop, or additional relay daemon.
The attached data path remains OpenSSH/tmux; this optimizes attachment overhead,
not model generation speed or network propagation time.

Use an absolute `REMOTE_CLAUDE_BIN` if the SSH non-login PATH omits Claude.
`REMOTE_USER` is the direct SSH login user; the native helper never uses `su`.
Config accepts plain `KEY=value` and paired quotes; it does not evaluate shell
expansions, source scripts, or expand `~`. Environment variables override file
values. Set `REMOTE_TMUX_SESSION` explicitly to reuse a Bash client's session;
the native default uses a different in-process hash. `CLAUDE_PROXY_SSH_CONFIG`
optionally selects an SSH config file. Run `disconnect` to close the master.

Network tuning and measurement are documented in [performance notes](docs/performance.md).
`native/claude-remote-proxy-native network-test` measures 50 request/reply
round trips through authenticated SSH, without starting Claude or tmux.

Measured on the owner's Singapore Tencent instance (2026-10-15, Ubuntu 26.04,
2 vCPU / 2 GB), using real loopback SSH and tmux with an already running test
process, 20 measured warm attachments per client after one discarded warm-up:

| Client | Median ready time | p95 ready time |
| --- | ---: | ---: |
| Bash remote mode | 68.70 ms | 71.81 ms |
| Native C++ mode | 16.47 ms | 17.70 ms |

This run reduced median attachment overhead by 52.23 ms (76%). These are
sequential local-loopback samples, not Internet latency or Claude response
benchmarks; machine load and SSH/shell configuration affect the result.

### Validation

Run `bash tests/test.sh` and `bash tests/remote.sh` for local regression tests.
Run `make -C native && bash tests/native.sh` for native regression tests.
On a disposable Linux server with SSH server, tmux, a C++ compiler and shasum,
run `sudo bash tests/integration.sh`. It starts a separate SSH daemon bound
only to loopback with temporary host/client keys and strict host verification.
It checks initial launch, process survival after a disconnected client,
reattachment without a duplicate process, and restart with the original UUID.
The Claude executable is a deterministic test stand-in: no authentication or
model calls are performed. A C++ PTY harness also measures 20 warm attachments
per client, from process launch until the tmux ready marker is displayed.
Test processes, keys, workspace and session state
are cleaned up; the system SSH configuration is not changed.

This integration test passed on the owner's Tencent Cloud Singapore instance
(Ubuntu Server 26.04, 2 vCPU / 2 GB). This validates SSH/tmux session lifecycle,
not Claude subscription authentication, model latency, or company networking.

On Windows use WSL with the repository in the Linux filesystem where possible.
Shell files are checked out as LF through `.gitattributes`.

Run Claude Code on a remote machine while keeping its terminal fully visible
and interactive on your local machine.

This is not an API wrapper. On the first run it synchronizes the current
workspace and one local Claude Code session. It then starts Claude inside a
remote tmux session and keeps attaching your terminal to that same process.
After attachment, input and output are transported unchanged.

## Why

Sometimes the useful context is local but the right execution environment is
remote: a larger Linux box, a disposable sandbox, a machine with private
services, or the exact build toolchain. A normal series of `ssh host command`
calls loses the interactive TUI and repeatedly reconnects. Copying only the
repository also loses the active Claude conversation.

Claude Remote Proxy keeps all four pieces together:

- workspace files are copied with `rsync`;
- the selected Claude Code JSONL session and project memory are placed under
  the remote user's correctly encoded Claude project directory exactly once;
- SSH ControlMaster provides one long-lived transport, and `ssh -tt` carries
  the remote TUI directly back to the local terminal;
- tmux owns the one long-lived Claude process, so a wrapper restart or dropped
  SSH client attaches again instead of starting or forking another session.

The proxy never runs `claude login`, types credentials, accepts workspace
trust, or answers permission prompts. Those decisions remain with the person
at the terminal.

## Requirements

Local machine:

- Bash 3.2+
- OpenSSH
- rsync
- an existing local Claude Code project session

Remote machine:

- OpenSSH server
- tmux
- Claude Code installed for the target user
- `su` when `SSH_TARGET` and `REMOTE_USER` are different accounts

SSH key authentication should already work before using the proxy.

## Quick start

```bash
git clone https://github.com/baryhuang/claude-remote-proxy.git
cd claude-remote-proxy
chmod +x bin/claude-remote-proxy

cp examples/proxy.env.example /path/to/my-project/.claude-remote-proxy.env
cd /path/to/my-project
/path/to/claude-remote-proxy/bin/claude-remote-proxy status
/path/to/claude-remote-proxy/bin/claude-remote-proxy start
```

On the first invocation, `start` performs three operations:

1. reports the transport, remote path, Claude version, and permission mode;
2. synchronizes the workspace and active Claude session;
3. starts one remote Claude process inside tmux and attaches the local PTY.

Every later invocation detects the persisted remote session and skips both
workspace sync and context import. If the tmux session is still running, it
attaches to the existing Claude process—there is no second `claude` command and
no resume prompt caused by the proxy. If the Claude process was intentionally
exited or the remote machine rebooted, the proxy starts it again using the same
session UUID, without forking.

If Claude asks whether to trust the workspace or sign in, answer it yourself.
The proxy deliberately does not automate either action.

## Commands

```text
connect       Open the persistent SSH control connection
status        Report both SSH and the persistent Claude process
sync          First import, or an explicit later workspace sync
run           Create once or attach to the same remote Claude process
start         Bootstrap if needed; otherwise attach without syncing
disconnect    Close SSH while leaving remote Claude running
encode-path   Show Claude Code's project-directory key for a path
```

The SSH master uses `ControlPersist`, so `status`, `sync`, and `run` create
separate SSH channels over the same TCP connection. The interactive channel
stays open while attached. tmux keeps Claude alive if that client disappears;
the next `run` or `start` attaches to it. There is no polling loop and no
command-by-command SSH reconnection.

To detach deliberately without ending Claude, use tmux's `Ctrl-b d`. Running
`disconnect` later closes the idle SSH master only; it never kills tmux or
Claude.

## Full-permission mode

For a disposable, trusted sandbox, set:

```bash
CLAUDE_PERMISSION_MODE="bypass"
```

This passes Claude Code's `--dangerously-skip-permissions` option. It does not
accept workspace trust and it does not authenticate Claude. Do not enable it
on a machine or repository you do not fully trust.

## What is synchronized

The workspace is copied without deletion during the initial bootstrap or an
explicit `sync`. `.claude-remote-proxy.env` is always excluded so host details
do not reach the remote project accidentally. Automatic reconnects never sync
files, because doing so could overwrite work produced by remote Claude.

For Claude context, the proxy selects the newest JSONL session in:

```text
~/.claude/projects/<encoded-local-project>/
```

Set `CLAUDE_SESSION_ID` to choose a specific session. On the remote side, the
session is installed under the target user's home using the *remote* project
path key, then owned by that user. A small remote state file records that UUID.
Later syncs preserve the remote JSONL instead of replacing its newer history.

Claude is launched with `--resume <same-uuid>`. The proxy intentionally never
passes `--fork-session`, because that option creates a new session ID. More
importantly, reconnecting to a live tmux session does not launch Claude at all.

See [How it works](docs/how-it-works.md) for the path mapping and the failure
modes discovered while building the first proxy session.

## Security boundaries

- No passwords, private keys, tokens, hostnames, or session IDs belong in this
  repository.
- The local config is sourced as shell code; treat it as a trusted file and
  keep it uncommitted.
- The SSH host key is verified by OpenSSH. Never blindly remove a stale key;
  verify the replacement fingerprint out of band first.
- Root SSH access is optional. When used, it is only the transport account;
  Claude runs as `REMOTE_USER`.
- Session JSONL files can contain sensitive conversation context. Send them
  only to a machine you control.

## License

MIT
