#!/usr/bin/env bash
# Disposable root-only loopback test. No model calls or production SSH changes.
set -Eeuo pipefail
[[ $EUID == 0 ]] || { echo 'Run on a disposable Linux server as root' >&2; exit 1; }
root=$(cd "$(dirname "$0")/.." && pwd)
for tool in ssh sshd ssh-keygen tmux c++; do command -v "$tool" >/dev/null; done
base=$(mktemp -d /tmp/ccproxy-integration.XXXXXX)
session=ccproxy-test-$$
state="$HOME/.claude/remote-proxy/$session.session-id"
project="$base/project"
key=$(printf '%s' "$project" | LC_ALL=C sed 's/[^A-Za-z0-9-]/-/g')
daemon=''
proxy_pid=''
cleanup() {
  tmux kill-session -t "$session" 2>/dev/null || :
  /usr/bin/ssh -F "$base/ssh_config" -S "$base/control.sock" -O exit test-company 2>/dev/null || :
  [[ -z $daemon ]] || { kill "$daemon" 2>/dev/null || :; wait "$daemon" 2>/dev/null || :; }
  [[ -z $proxy_pid ]] || { kill "$proxy_pid" 2>/dev/null || :; wait "$proxy_pid" 2>/dev/null || :; }
  rm -f "$state" "$state.lock"
  rm -rf -- "$base" "$HOME/.claude/projects/$key"
}
trap cleanup EXIT
mkdir "$project" "$base/bin"
ssh-keygen -q -t ed25519 -N '' -f "$base/key"
ssh-keygen -q -t ed25519 -N '' -f "$base/host"
# Let the kernel choose a free loopback port, without a Python dependency.
c++ -O2 "$root/tests/terminal.cpp" -lutil -o "$base/terminal"
port=$("$base/terminal" --port)
cat >"$base/sshd_config" <<EOF
Port $port
ListenAddress 127.0.0.1
HostKey $base/host
PidFile $base/sshd.pid
AuthorizedKeysFile $base/key.pub
StrictModes no
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitRootLogin prohibit-password
UsePAM no
AllowUsers root
EOF
read -r type pub _ <"$base/host.pub"
printf '[127.0.0.1]:%s %s %s\n' "$port" "$type" "$pub" >"$base/known_hosts"
cat >"$base/ssh_config" <<EOF
Host test-company
 HostName 127.0.0.1
 Port $port
 User root
 IdentityFile $base/key
 IdentitiesOnly yes
 StrictHostKeyChecking yes
 UserKnownHostsFile $base/known_hosts
 BatchMode yes
EOF
printf '#!/bin/sh\nexec /usr/bin/ssh -F "%s/ssh_config" "$@"\n' "$base" >"$base/bin/ssh"
cat >"$base/claude-test" <<EOF
#!/bin/bash
set -eu
id=\${!#}
mkdir -p "\$HOME/.claude/projects/$key"
touch "\$HOME/.claude/projects/$key/\$id.jsonl"
printf '%s %s %s\n' "\$\$" "\$1" "\$id" >> "$base/events"
printf '%s|%s|%s|%s\n' "\$HTTPS_PROXY" "\$https_proxy" "\$NO_PROXY" "\$no_proxy" >> "$base/api-env"
printf 'TEST_CLAUDE_READY\n'
exec sleep infinity
EOF
chmod +x "$base/bin/ssh" "$base/claude-test"
export PATH="$base/bin:$PATH" TERM=xterm-256color CLAUDE_PROXY_MODE=remote
export CLAUDE_PROXY_PROJECT_DIR="$project" SSH_TARGET=test-company REMOTE_USER=root
export REMOTE_PROJECT_DIR="$project" REMOTE_TMUX_SESSION="$session"
export REMOTE_CLAUDE_BIN="$base/claude-test" CLAUDE_PROXY_SOCKET="$base/control.sock"
export REMOTE_PROXY_BIN="$root/native/claude-remote-proxy-native"
/usr/sbin/sshd -D -f "$base/sshd_config" -E "$base/sshd.log" &
daemon=$!
for ((i=0;i<100;i++)); do
  [[ -e $base/sshd.pid ]] && break
  sleep .05
done
env -u CLAUDE_PROXY_SOCKET "$REMOTE_PROXY_BIN" network-test
env -u CLAUDE_PROXY_SOCKET "$REMOTE_PROXY_BIN" disconnect
c++ -O2 -Wall -Wextra "$root/tests/http_proxy.cpp" -o "$base/http-proxy"
"$base/http-proxy" "$base/proxy.port" &
proxy_pid=$!
for ((i=0;i<100;i++)); do [[ -s $base/proxy.port ]] && break; sleep .05; done
proxy="127.0.0.1:$(cat "$base/proxy.port")"
env -u CLAUDE_PROXY_SOCKET SSH_HTTP_PROXY="$proxy" "$REMOTE_PROXY_BIN" network-test
env -u CLAUDE_PROXY_SOCKET SSH_HTTP_PROXY="$proxy" "$REMOTE_PROXY_BIN" disconnect
env -u CLAUDE_PROXY_SOCKET SSH_COMPRESSION=yes SSH_IP_QOS=none SSH_CIPHERS=aes128-gcm@openssh.com SSH_OBSCURE_KEYSTROKE_TIMING=no "$REMOTE_PROXY_BIN" network-test
env -u CLAUDE_PROXY_SOCKET SSH_COMPRESSION=yes SSH_IP_QOS=none SSH_CIPHERS=aes128-gcm@openssh.com SSH_OBSCURE_KEYSTROKE_TIMING=no "$REMOTE_PROXY_BIN" disconnect
"$REMOTE_PROXY_BIN" network-test
export REMOTE_API_PROXY=http://127.0.0.1:1085 REMOTE_API_NO_PROXY=internal.example
# No listener exists on 1085: this still succeeds because API routing never
# changes the SSH transport or the credential-free echo diagnostic.
"$REMOTE_PROXY_BIN" network-test
"$base/terminal" --drop "$REMOTE_PROXY_BIN" start
read -r pid flag id <"$base/events"
[[ $flag == --session-id ]]
tmux has-session -t "$session"
grep -Fxq 'http://127.0.0.1:1085|http://127.0.0.1:1085|localhost,127.0.0.1,::1,internal.example|localhost,127.0.0.1,::1,internal.example' "$base/api-env"
export REMOTE_API_PROXY=http://127.0.0.1:1086
"$base/terminal" --drop "$REMOTE_PROXY_BIN" start
[[ $(wc -l <"$base/events") == 1 ]]
[[ $(wc -l <"$base/api-env") == 1 ]]
kill -0 "$pid"
tmux kill-session -t "$session"
"$base/terminal" --drop "$REMOTE_PROXY_BIN" start
read -r _ flag2 id2 < <(tail -n 1 "$base/events")
[[ $flag2 == --resume && $id == "$id2" ]]
tail -1 "$base/api-env" | grep -Fq 'http://127.0.0.1:1086|http://127.0.0.1:1086|'
"$base/terminal" --bench bash "$root/bin/claude-remote-proxy" start
"$base/terminal" --bench "$REMOTE_PROXY_BIN" start
[[ $(wc -l <"$base/events") == 2 ]]
echo 'PASS: real SSH/tmux; disconnect preserves process; restart resumes UUID; zero model calls'
