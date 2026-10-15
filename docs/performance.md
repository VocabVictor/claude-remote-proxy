# Performance and network transport

There are separate paths to optimize:

1. Client to company server: interactive input, redraws, and SSH reconnection.
2. Company server to model service (possibly through a Singapore exit): API
   connection establishment, streaming, and proxy queueing.
3. Company server itself: Claude, project tools, tmux, and terminal redraws.

Claude must run on the company server to use its workspace. Routing its API
traffic through Singapore does not require routing the terminal connection
through Singapore too. Measure these paths separately before adding hops.

## Measure SSH independently

Run `native/claude-remote-proxy-native network-test` with the normal config and
the matching native binary installed at `REMOTE_PROXY_BIN` on the remote host.
Key/agent authentication and previously verified host keys are required;
the diagnostic uses BatchMode and will not ask for a password or host-key
approval. It reports first-response time, then p50/p95/max for 50 sequential
one-byte echoes. First response includes SSH channel setup and, for a cold
master, connection/authentication. Subsequent samples measure the SSH channel
and remote echo scheduling, excluding tmux, the terminal renderer and Claude.
This is not a bandwidth or packet-loss test. A failed/contaminated probe exits
with an error rather than reporting a latency result.

The normal start command still replaces itself with SSH; probe pipes and
measurement code are never part of the interactive forwarding path.

## Transport options

Optional config values map directly to OpenSSH options:

| Variable | Purpose |
| --- | --- |
| `SSH_COMPRESSION=yes` or `no` | Compare bandwidth savings with compression CPU/delay. |
| `SSH_IP_QOS=none` | Test paths that handle DSCP traffic marking poorly. |
| `SSH_CIPHERS=aes128-gcm@openssh.com` | Compare with `chacha20-poly1305@openssh.com` on the actual CPU. |
| `SSH_OBSCURE_KEYSTROKE_TIMING=no` | Disable interactive timing masking on OpenSSH 9.5+. |

Unset values inherit the user's SSH configuration. Cipher availability is
checked by OpenSSH. No weak cipher or host-key-verification bypass is added.
Timing masking normally uses 20 ms packet intervals; disabling it can reduce
intentional input scheduling delay but reveals typing timing to passive
traffic observers. It does not remove network RTT. See the
[OpenSSH manual](https://man.openbsd.org/ssh_config#ObscureKeystrokeTiming).
The non-TTY `network-test` does not measure this interactive-only feature.

Default master sockets include OpenSSH's resolved host/port/user hash and
a transport-profile suffix. Changing these options therefore creates a new
master instead of silently reusing the old transport settings. Old masters
expire according to ControlPersist. If `CLAUDE_PROXY_SOCKET` is explicitly
overridden, run `disconnect` before changing options and reconnecting.

Use this terminal's dedicated master for interactive channels; do not attach
large SCP transfers or API tunnels to the same socket. Shared SSH channels
share one TCP connection; packet loss or queued bulk traffic can delay input.
Separate connections avoid that shared queue but still share physical link
capacity. Avoid TCP tunnels inside another congested TCP tunnel when a direct
SSH route is available.

## Observations on 2026-10-15

The owner's Singapore test server runs Ubuntu 26.04 / Linux 7.0.0-30 with
OpenSSH 10.2p1. The active TCP congestion control was cubic; available choices
were reno and cubic. `net.core.default_qdisc` reported fq_codel, but active
eth0 queues were pfifo_fast under mq. The configured default alone does not
prove the live interface uses it. No kernel, congestion-control, qdisc or
global network settings were changed.

Ten direct TCP-connect attempts from the Windows host to that server's port
22 produced nine successful measurements: 112.45–1253.98 ms, with a median
of 123.03 ms; one exceeded the 2500 ms timeout. This is a small observational
sample, including host scheduling and TCP setup. It is not SSH echo RTT,
proof of packet loss, or evidence about the company-server route.

For further tuning, compare unloaded and loaded p95 echo latency on the
actual route, then investigate queueing, route choice, MTU and retransmits.
Changing BBR, buffers or MTU without that evidence can fail to help or worsen
interactive latency. Model API keep-alive/streaming requires inspecting the
actual API proxy; this repository does not intercept those requests.
