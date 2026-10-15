#!/usr/bin/env bash
set -Eeuo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
binary="${CCPROXY_NATIVE_BIN:-$root/native/claude-remote-proxy-native}"
base=$(mktemp -d)
trap 'rm -rf -- "$base"' EXIT
mkdir -p "$base/bin" "$base/home" "$base/project with ' quote"
export HOME="$base/home" PATH="$base/bin:$PATH" CAPTURE="$base/capture"
printf '#!/bin/sh\nprintf "%%s\\n" "$@" > "$CAPTURE"\n' >"$base/bin/ssh"
cp "$base/bin/ssh" "$base/bin/tmux"
chmod +x "$base/bin/ssh" "$base/bin/tmux"
export SSH_TARGET=test REMOTE_PROJECT_DIR="$base/project with ' quote" REMOTE_PROXY_BIN="$binary"
export CLAUDE_PROXY_SOCKET="$base/control.sock"
export CLAUDE_PROXY_CONFIG="$base/config"
printf 'CLAUDE_PROXY_MODE=remote\n' >"$CLAUDE_PROXY_CONFIG"
"$binary" start
[[ $(head -n 1 "$CAPTURE") == -S ]]
! grep -q 'ControlMaster=yes' "$CAPTURE"
grep -qx 'ObscureKeystrokeTiming=no' "$CAPTURE"
grep -qx 'IgnoreUnknown=ObscureKeystrokeTiming' "$CAPTURE"
SSH_OBSCURE_KEYSTROKE_TIMING=yes "$binary" start
grep -qx 'ObscureKeystrokeTiming=yes' "$CAPTURE"
SSH_COMPRESSION=no SSH_IP_QOS=none SSH_CIPHERS=aes128-gcm@openssh.com SSH_OBSCURE_KEYSTROKE_TIMING=no "$binary" start
grep -qx 'Compression=no' "$CAPTURE"
grep -qx 'IPQoS=none' "$CAPTURE"
grep -qx 'Ciphers=aes128-gcm@openssh.com' "$CAPTURE"
grep -qx 'ObscureKeystrokeTiming=no' "$CAPTURE"
if SSH_COMPRESSION=invalid "$binary" start 2>/dev/null; then exit 1; fi
SSH_HTTP_PROXY=http://127.0.0.1:1080 "$binary" start
grep -qx 'ProxyUseFdpass=yes' "$CAPTURE"
grep -q -- --http-connect "$CAPTURE"
if SSH_HTTP_PROXY=http://user:password@127.0.0.1:1080 "$binary" start 2>/dev/null; then exit 1; fi
if SSH_HTTP_PROXY=127.0.0.1:0 "$binary" start 2>/dev/null; then exit 1; fi
REMOTE_API_PROXY=http://127.0.0.1:1085 REMOTE_API_NO_PROXY=internal.example "$binary" start
! grep -q -- --http-connect "$CAPTURE"
grep -q "http://127.0.0.1:1085" "$CAPTURE"
if REMOTE_API_PROXY=http://user:password@127.0.0.1:1085 "$binary" start 2>/dev/null; then exit 1; fi
if REMOTE_API_NO_PROXY=internal.example "$binary" start 2>/dev/null; then exit 1; fi
[[ $(printf 'echo-test' | "$binary" --echo) == echo-test ]]
env -u CLAUDE_PROXY_SOCKET HOME="$base" SSH_COMPRESSION=no "$binary" start
socket1=$(sed -n '2p' "$CAPTURE")
env -u CLAUDE_PROXY_SOCKET HOME="$base" SSH_COMPRESSION=yes "$binary" start
socket2=$(sed -n '2p' "$CAPTURE")
[[ $socket1 == *%C* && $socket1 != "$socket2" ]]
"$binary" --serve "$REMOTE_PROJECT_DIR" test-session "$base/claude with ' quote" standard
state="$HOME/.claude/remote-proxy/test-session.session-id"
id=$(cat "$state")
[[ ${#id} == 36 ]]
grep -q -- --session-id "$CAPTURE"
# Multiple initializers publish exactly one UUID; tmux itself arbitrates creation.
rm "$state"
for ((i=0;i<16;i++)); do
 CAPTURE="$base/capture.$i" "$binary" --serve "$REMOTE_PROJECT_DIR" test-session claude standard &
done
wait
id=$(cat "$state")
for ((i=0;i<16;i++)); do grep -q "$id" "$base/capture.$i"; done
key=$(printf '%s' "$REMOTE_PROJECT_DIR" | LC_ALL=C sed 's/[^A-Za-z0-9-]/-/g')
mkdir -p "$HOME/.claude/projects/$key"
touch "$HOME/.claude/projects/$key/$id.jsonl"
"$binary" --serve "$REMOTE_PROJECT_DIR" test-session claude standard
grep -q -- --resume "$CAPTURE"
! grep -q -- --dangerously "$CAPTURE"
if "$binary" --serve "$base/missing" test-session claude standard 2>/dev/null; then exit 1; fi
printf 'broken\n' >"$state"
if "$binary" --serve "$REMOTE_PROJECT_DIR" test-session claude standard 2>/dev/null; then exit 1; fi
echo 'native tests passed (quoting, permissions, resume, concurrent UUID initialization)'
