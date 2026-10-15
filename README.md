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
