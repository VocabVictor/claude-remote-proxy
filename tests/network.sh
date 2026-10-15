#!/usr/bin/env bash
# Controlled network benchmark. Only temporary network namespaces are shaped.
set -Eeuo pipefail
[[ $EUID == 0 ]] || { echo 'Run on a disposable Linux server as root' >&2; exit 1; }
root=$(cd "$(dirname "$0")/.." && pwd)
for tool in ip tc ssh sshd ssh-keygen c++; do command -v "$tool" >/dev/null; done
base=$(mktemp -d /tmp/ccproxy-net.XXXXXX)
client=ccproxy-c-$$
server=ccproxy-s-$$
vc=cpc$$
vs=cps$$
client_created=no
server_created=no
root_link_created=no
cleanup_namespace() {
  local ns=$1
  for pid in $(ip netns pids "$ns" 2>/dev/null); do kill "$pid" 2>/dev/null || :; done
  ip netns del "$ns" 2>/dev/null || :
}
cleanup() {
  # Never clean up a pre-existing object if creation failed on a name clash.
  if [[ $client_created == yes ]]; then cleanup_namespace "$client"; fi
  if [[ $server_created == yes ]]; then cleanup_namespace "$server"; fi
  if [[ $root_link_created == yes ]]; then ip link del "$vc" 2>/dev/null || :; fi
  rm -rf -- "$base"
}
trap cleanup EXIT
ip netns add "$client"
client_created=yes
ip netns add "$server"
server_created=yes
ip link add "$vc" type veth peer name "$vs"
root_link_created=yes
ip link set "$vc" netns "$client"
root_link_created=no
ip link set "$vs" netns "$server"
ip -n "$client" addr add 198.18.0.1/30 dev "$vc"
ip -n "$server" addr add 198.18.0.2/30 dev "$vs"
for ns in "$client" "$server"; do ip -n "$ns" link set lo up; done
ip -n "$client" link set "$vc" up
ip -n "$server" link set "$vs" up
ssh-keygen -q -t ed25519 -N '' -f "$base/key"
ssh-keygen -q -t ed25519 -N '' -f "$base/host"
read -r type pub _ <"$base/host.pub"
printf '[198.18.0.2]:22022 %s %s\n' "$type" "$pub" >"$base/known_hosts"
cat >"$base/sshd_config" <<EOF
Port 22022
ListenAddress 198.18.0.2
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
cat >"$base/ssh_config" <<EOF
Host test-net
 HostName 198.18.0.2
 Port 22022
 User root
 IdentityFile $base/key
 IdentitiesOnly yes
 StrictHostKeyChecking yes
 UserKnownHostsFile $base/known_hosts
 BatchMode yes
 Compression no
 ControlMaster auto
 ControlPersist 60s
 ConnectTimeout 10
EOF
ip netns exec "$server" /usr/sbin/sshd -D -f "$base/sshd_config" -E "$base/sshd.log" &
for ((i=0;i<100;i++)); do [[ -e $base/sshd.pid ]] && break; sleep .05; done
c++ -O2 -Wall -Wextra "$root/tests/terminal.cpp" -lutil -o "$base/terminal"
helper="$root/native/claude-remote-proxy-native"
export CCPROXY_RTT_SAMPLES=20
# The remote command is shell-quoted; test paths are generated under /tmp.
remote="exec '$helper' --echo"
ssh_base=(ssh -F "$base/ssh_config")
measure() {
  local socket=$1 obscure=$2
  ip netns exec "$client" "$base/terminal" --rtt "${ssh_base[@]}" -S "$socket" \
    -o "ObscureKeystrokeTiming=$obscure" -tt test-net "$remote"
}
close_master() {
  ip netns exec "$client" "${ssh_base[@]}" -S "$1" -O exit test-net 2>/dev/null || :
}
echo 'SCENARIO loopback-veth: no injected delay; timing masking yes / no'
measure "$base/timing-yes" yes
close_master "$base/timing-yes"
measure "$base/timing-no" no
close_master "$base/timing-no"
# 40 ms base RTT, 5 Mbit/s, up to 1000 queued packets per direction.
ip netns exec "$client" tc qdisc add dev "$vc" root handle 1: netem delay 20ms rate 5mbit limit 1000
ip netns exec "$server" tc qdisc add dev "$vs" root handle 1: netem delay 20ms rate 5mbit limit 1000
echo 'SCENARIO shaped: 40ms base RTT / 5Mbit / FIFO queue, no bulk'
measure "$base/idle" no
close_master "$base/idle"
bulk() {
  ip netns exec "$client" "${ssh_base[@]}" -S "$1" -o ObscureKeystrokeTiming=no \
    -T test-net 'exec dd if=/dev/zero bs=65536 count=1024 status=none' >/dev/null &
  bulk_pid=$!
  sleep 1
}
stop_bulk() {
  kill "$bulk_pid" 2>/dev/null || :
  wait "$bulk_pid" 2>/dev/null || :
}
echo 'SCENARIO shaped FIFO: bulk and terminal share one SSH master'
bulk "$base/shared"
measure "$base/shared" no
stop_bulk
close_master "$base/shared"
echo 'SCENARIO shaped FIFO: separate masters (same physical queue)'
bulk "$base/bulk"
measure "$base/interactive" no
stop_bulk
close_master "$base/bulk"
close_master "$base/interactive"
# Compare fair queueing only inside isolated namespaces, never eth0.
ip netns exec "$client" tc qdisc add dev "$vc" parent 1:1 handle 10: fq_codel
ip netns exec "$server" tc qdisc add dev "$vs" parent 1:1 handle 10: fq_codel
echo 'SCENARIO shaped fq_codel: separate masters'
bulk "$base/bulk-fq"
measure "$base/interactive-fq" no
stop_bulk
close_master "$base/bulk-fq"
close_master "$base/interactive-fq"
echo 'PASS: controlled SSH input test; zero model calls; production interfaces unchanged'
