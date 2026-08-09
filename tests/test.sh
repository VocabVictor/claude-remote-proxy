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
  'claude-remote-proxy 0.1.0' \
  "$($PROXY version)"

$PROXY help | grep -q 'persistent SSH control connection'

printf 'all tests passed\n'
