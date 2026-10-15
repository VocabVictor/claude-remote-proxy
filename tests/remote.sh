#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PROXY="$ROOT/bin/claude-remote-proxy"
FIXTURE=$(mktemp -d)
trap 'rm -rf "$FIXTURE"' EXIT
mkdir -p "$FIXTURE/bin" "$FIXTURE/home" "$FIXTURE/project"
export HOME="$FIXTURE/home"
export PROBE_LOG="$FIXTURE/ssh.log" TMUX_LOG="$FIXTURE/tmux.log"
export CLAUDE_PROXY_PROJECT_DIR="$FIXTURE/project"
export SSH_TARGET=company-test REMOTE_USER="$(id -un)"
export REMOTE_PROJECT_DIR="$FIXTURE/project" CLAUDE_PROXY_MODE=remote
export CLAUDE_PROXY_SOCKET="$FIXTURE/control.sock"
cat > "$FIXTURE/bin/ssh" <<'EOF'
#!/bin/bash
printf 'call\n' >> "$PROBE_LOG"
case "$*" in *'-O check'*) exit 0;; esac
exec /bin/bash -c "${!#}"
EOF
# Preserve the isolated HOME rather than starting the test user's login shell.
cat > "$FIXTURE/bin/bash" <<'EOF'
#!/bin/bash
if [[ ${1:-} == -lc ]]; then
  exec /bin/bash --noprofile --norc -c "$2"
fi
exec /bin/bash "$@"
EOF
cat > "$FIXTURE/bin/tmux" <<'EOF'
#!/bin/bash
printf '%s\n' "$*" >> "$TMUX_LOG"
launch=${!#}
uuid=$(printf '%s' "$launch" | grep -Eo '[0-9a-f-]{36}')
key=$(printf '%s\n' "$REMOTE_PROJECT_DIR" | sed 's/[^A-Za-z0-9-]/-/g')
mkdir -p "$HOME/.claude/projects/$key"
touch "$HOME/.claude/projects/$key/$uuid.jsonl"
EOF
cat > "$FIXTURE/bin/claude" <<'EOF'
#!/bin/bash
exit 0
EOF
for tool in rsync su; do
  printf '#!/bin/bash\nexit 99\n' > "$FIXTURE/bin/$tool"
done
chmod +x "$FIXTURE/bin/"*
export PATH="$FIXTURE/bin:$PATH"
"$PROXY" start
[[ $(wc -l < "$PROBE_LOG") -eq 2 ]]
grep -q -- '--session-id' "$TMUX_LOG"
[[ $(find "$HOME/.claude/remote-proxy" -name '*.session-id' | wc -l) -eq 1 ]]
uuid=$(cat "$HOME/.claude/remote-proxy/"*.session-id)
"$PROXY" start
[[ $(wc -l < "$PROBE_LOG") -eq 4 ]]
tail -1 "$TMUX_LOG" | grep -q -- "--resume $uuid"
if "$PROXY" sync 2>/dev/null; then
  printf 'remote sync must be refused\n' >&2; exit 1
fi
[[ $(wc -l < "$PROBE_LOG") -eq 4 ]]
# Missing workspaces must fail without creating or changing a project directory.
export REMOTE_PROJECT_DIR="$FIXTURE/missing"
if "$PROXY" start 2>/dev/null; then
  printf 'missing remote project must fail\n' >&2; exit 1
fi
[[ ! -e $REMOTE_PROJECT_DIR ]]
printf 'remote mode tests passed\n'
