# How it works

## Data and terminal flow

```text
local project ──first-run rsync─────────▶ remote project
local session ──one-time import─────────▶ remote ~/.claude/projects/<key>
local terminal ◀──persistent SSH/PTTY──▶ remote tmux ──▶ one Claude process
```

The file-copy commands and the interactive PTY are SSH channels multiplexed
over one ControlMaster connection. `ssh -tt` is important: without a PTY,
Claude Code does not behave like the local terminal UI and prompts cannot be
relayed faithfully. Server-alive messages keep the master healthy while it is
idle.

tmux is a second, separate persistence layer on the remote machine. The tmux
server owns Claude, not the SSH client. Re-running the proxy attaches to the
named tmux session; it does not execute another Claude command. This remains
true even when a dropped network connection requires a new SSH transport.

## The remote user matters

The transport login and process identity can be different. A common sandbox
arrangement permits SSH as root while Claude's existing settings,
authentication, and home directory belong to an unprivileged account. The
proxy therefore uses root only to prepare ownership and then runs:

```text
su - REMOTE_USER -c 'cd REMOTE_PROJECT_DIR && exec claude ...'
```

Using a login shell (`su -`) supplies that user's home and environment.

## Claude project path encoding

Claude stores per-project sessions below `~/.claude/projects`, but the
directory name is derived from the absolute project path. Non-alphanumeric
characters other than hyphens become hyphens. For example:

```text
/home/dev_user/work/my_app
        ↓
-home-dev-user-work-my-app
```

This mapping must use the *remote* absolute path. Copying a session to a key
derived from the local path—or preserving underscores—makes `--resume` report
that the conversation cannot be found even though the JSONL file exists.

The helper can show the expected key:

```bash
claude-remote-proxy encode-path /home/dev_user/work/my_app
```

## Session handoff

The JSONL conversation is copied from the matching local Claude project
directory to the remote one only during initialization. Project memory is also
imported once. The proxy writes the chosen UUID to:

```text
~/.claude/remote-proxy/<tmux-name>.session-id
```

That state file prevents a later local sync from replacing the remote JSONL,
which may already contain newer turns. Claude is initially started with:

```text
--resume <session-id>
```

`--fork-session` is deliberately not used. Claude documents that flag as
creating a new session ID when resuming—the opposite of a persistent proxy.
While tmux is alive, reconnecting does not run even the `--resume` command; it
only attaches to the process already holding that conversation.

If the initial imported conversation is large, Claude may offer or
automatically run `/compact`. That is Claude's own context-management
operation; the proxy only relays its screen updates. It is not repeated merely
because the local SSH client reconnects.

## Authentication and trust

The proxy treats both as human-controlled boundaries:

- it never invokes `claude login`;
- it never sends keystrokes to a sign-in screen;
- it never selects workspace trust;
- it never changes Claude's permission mode after launch.

The configured permission mode only determines whether the documented
`--dangerously-skip-permissions` launch flag is present. Any subsequent prompt
is shown directly to the local operator.

## Reconnect versus restart

These two operations are intentionally different:

- **Reconnect:** `start` or `run` finds the named tmux session and attaches.
  Claude is still the same process and session; nothing is resumed or synced.
- **Restart:** Claude was explicitly exited, tmux ended, or the host rebooted.
  The proxy launches Claude again with the persisted UUID and no fork. A Claude
  resume/compaction choice may be legitimate in this case because the process
  really did restart.

An explicit `sync` is refused while the tmux session is running. This avoids
overwriting files underneath an active agent.

## Troubleshooting

### Host key verification failed

Compare the server's current public host-key fingerprint through a trusted
console or provider control plane. Only after it matches should the stale
`known_hosts` entry be replaced. Do not disable host-key checking.

### `--resume` cannot find the conversation

Check all four values together:

1. the remote process runs as the intended user;
2. the JSONL is below that user's home;
3. the project key is encoded from the remote absolute path;
4. the JSONL filename is `<session-id>.jsonl`.

### Every reconnect starts a new conversation

Run `status` and confirm that `remote tmux` says `running; next start will
attach`. Also confirm the launch command contains no `--fork-session`. If tmux
is absent, install it on the remote host; SSH ControlMaster alone preserves a
network connection, not the Claude process after a client exits.

### Claude opens but lacks tools or settings

Confirm that `su - REMOTE_USER` resolves the expected home and PATH. Claude
must be installed and authenticated for that user, not merely for the SSH
transport account.

### Auto-update cannot write to the npm prefix

This is independent of the proxy. Run `claude doctor` manually as the remote
user and fix that user's installation ownership. The proxy intentionally does
not elevate or repair Claude itself.
