#!/usr/bin/env bash

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PROXY="$ROOT/bin/claude-remote-proxy"

assert_eq() {
  local expected=$1 actual=$2
  if [[ $expected != "$actual" ]]; then
    printf 'expected: %s\nactual:   %s\n' "$expected" "$actual" >&2
    exit 1
  fi
}

assert_eq \
  '-home-dev-user-work-my-app' \
  "$($PROXY encode-path /home/dev_user/work/my_app)"

assert_eq \
  '-Users-example-My-Project' \
  "$($PROXY encode-path '/Users/example/My Project')"

assert_eq \
  'claude-remote-proxy 0.2.0' \
  "$($PROXY version)"

$PROXY help | grep -q 'persistent SSH control connection'

if grep -Eq 'inner\+?=.*--fork-session' "$PROXY"; then
  printf '%s\n' 'proxy must never fork the imported Claude session' >&2
  exit 1
fi

grep -q 'tmux attach-session' "$PROXY"
grep -q 'remote session already exists; skipping workspace and context sync' "$PROXY"

# A subsequent `start` must attach to the live tmux session. This fake SSH
# endpoint reports an initialized/running remote and records every command.
FIXTURE=$(mktemp -d)
trap 'rm -rf "$FIXTURE"' EXIT
mkdir -p "$FIXTURE/bin" "$FIXTURE/project"
SSH_LOG="$FIXTURE/ssh.log"

printf '%s\n' \
  '#!/usr/bin/env bash' \
  'printf "%s\\n" "$*" >> "$FAKE_SSH_LOG"' \
  'case "$*" in' \
  '  *"-O check"*) exit 0 ;;' \
  '  *"getent passwd"*) printf "/home/tester\\n" ;;' \
  '  *"claude --version"*) printf "2.1.0\\n" ;;' \
  '  *"command -v claude"*) printf "/usr/bin/claude\\n" ;;' \
  '  *"command -v tmux"*) printf "/usr/bin/tmux\\n" ;;' \
  '  *"tmux has-session"*) [[ ${FAKE_TMUX_RUNNING:-1} == 1 ]] ;;' \
  '  *"test -s"*"cat"*) printf "11111111-1111-1111-1111-111111111111\\n" ;;' \
  '  *"test -s"*) exit 0 ;;' \
  '  *"if test -d"*) printf "present" ;;' \
  'esac' \
  > "$FIXTURE/bin/ssh"

printf '%s\n' \
  '#!/usr/bin/env bash' \
  'printf "rsync must not run during attach\\n" >&2' \
  'exit 99' \
  > "$FIXTURE/bin/rsync"
chmod +x "$FIXTURE/bin/ssh" "$FIXTURE/bin/rsync"

env \
  PATH="$FIXTURE/bin:$PATH" \
  FAKE_SSH_LOG="$SSH_LOG" \
  CLAUDE_PROXY_PROJECT_DIR="$FIXTURE/project" \
  CLAUDE_PROXY_SOCKET="/tmp/claude-proxy-test-$$.sock" \
  SSH_TARGET="root@example.test" \
  REMOTE_USER="tester" \
  REMOTE_PROJECT_DIR="/home/tester/project" \
  "$PROXY" start >/dev/null 2>&1

grep -q 'tmux attach-session' "$SSH_LOG"
if grep -q 'tmux new-session\|exec claude' "$SSH_LOG"; then
  printf '%s\n' 'reconnect started a second Claude process' >&2
  exit 1
fi

# If the process really ended, use the persisted UUID without forking it.
: > "$SSH_LOG"
env \
  PATH="$FIXTURE/bin:$PATH" \
  FAKE_SSH_LOG="$SSH_LOG" \
  FAKE_TMUX_RUNNING=0 \
  CLAUDE_PROXY_PROJECT_DIR="$FIXTURE/project" \
  CLAUDE_PROXY_SOCKET="/tmp/claude-proxy-test-$$.sock" \
  SSH_TARGET="root@example.test" \
  REMOTE_USER="tester" \
  REMOTE_PROJECT_DIR="/home/tester/project" \
  "$PROXY" run >/dev/null 2>&1

grep -q 'tmux new-session' "$SSH_LOG"
grep -q -- '--resume.*11111111-1111-1111-1111-111111111111' "$SSH_LOG"
if grep -q -- '--fork-session' "$SSH_LOG"; then
  printf '%s\n' 'real restart forked the persisted Claude session' >&2
  exit 1
fi

printf 'all tests passed\n'
