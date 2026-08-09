# Claude Remote Proxy

Run Claude Code on a remote machine while keeping its terminal fully visible
and interactive on your local machine.

This is not an API wrapper. It synchronizes the current workspace and a local
Claude Code session, opens one persistent SSH connection, changes to the
intended remote OS user, and attaches your terminal directly to the remote
Claude process. After attachment, input and output are transported unchanged.

## Why

Sometimes the useful context is local but the right execution environment is
remote: a larger Linux box, a disposable sandbox, a machine with private
services, or the exact build toolchain. A normal series of `ssh host command`
calls loses the interactive TUI and repeatedly reconnects. Copying only the
repository also loses the active Claude conversation.

Claude Remote Proxy keeps all three pieces together:

- workspace files are copied with `rsync`;
- the selected Claude Code JSONL session and project memory are placed under
  the remote user's correctly encoded Claude project directory;
- SSH ControlMaster provides one long-lived transport, and `ssh -tt` carries
  the remote TUI directly back to the local terminal.

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

`start` performs three operations:

1. reports the transport, remote path, Claude version, and permission mode;
2. synchronizes the workspace and active Claude session;
3. replaces the local wrapper process with an interactive remote Claude PTY.

If Claude asks whether to trust the workspace or sign in, answer it yourself.
The proxy deliberately does not automate either action.

## Commands

```text
connect       Open the persistent SSH control connection
status        Check transport and prerequisites without logging into Claude
sync          Copy workspace, selected session, and project memory
run           Attach to remote Claude without syncing
start         Run status, sync, and run in order
disconnect    Explicitly close the persistent SSH connection
encode-path   Show Claude Code's project-directory key for a path
```

The SSH master uses `ControlPersist`, so `status`, `sync`, and `run` create
separate SSH channels over the same TCP connection. The interactive channel
stays open for the life of Claude. There is no polling loop and no command-by-
command SSH reconnection.

## Full-permission mode

For a disposable, trusted sandbox, set:

```bash
CLAUDE_PERMISSION_MODE="bypass"
```

This passes Claude Code's `--dangerously-skip-permissions` option. It does not
accept workspace trust and it does not authenticate Claude. Do not enable it
on a machine or repository you do not fully trust.

## What is synchronized

The workspace is copied without deletion. `.claude-remote-proxy.env` is always
excluded so host details do not reach the remote project accidentally.

For Claude context, the proxy selects the newest JSONL session in:

```text
~/.claude/projects/<encoded-local-project>/
```

Set `CLAUDE_SESSION_ID` to choose a specific session. On the remote side, the
session is installed under the target user's home using the *remote* project
path key, then owned by that user. Claude is launched with `--resume` and
`--fork-session`, preserving the copied history while allowing the remote run
to continue independently.

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
