# Performance and network transport

There are separate paths to optimize:

1. Client to company server: interactive input, redraws, and SSH reconnection.
2. Company server to model service (possibly through a Singapore exit): API
   connection establishment, streaming, and proxy queueing.
3. Company server itself: Claude, project tools, tmux, and terminal redraws.

Claude must run on the company server to use its workspace. Routing its API
traffic through Singapore does not require routing the terminal connection
through Singapore too. Measure these paths separately before adding hops.

## Separate terminal and Claude HTTP routing

Native 0.5.0 adds `REMOTE_API_PROXY=http://host:port` and optional
`REMOTE_API_NO_PROXY=comma,separated,hosts`. The address is resolved on the
**company server**, not on your laptop or the Singapore origin. It must be
an existing HTTP CONNECT endpoint; this setting neither creates a proxy nor
selects its exit country. Credential-bearing URLs and paths are rejected.
For a private Singapore exit, connect to its proxy using an authenticated,
encrypted transport rather than exposing an unauthenticated Internet proxy.

The remote native helper adds HTTP_PROXY/HTTPS_PROXY and lowercase variants
only to the newly launched Claude process and its descendants. It leaves
SSH routing, the tmux server's global environment, other sessions and other
applications alone. It removes ALL_PROXY variants only in that child.
HTTP requests made by Claude's tools can also inherit this environment:
this is process-scoped routing, not hostname-exclusive API interception.
Use the bypass list for company/private and domestic tool endpoints.

Loopback destinations are always bypassed. If an explicit bypass list is
provided, it replaces the non-loopback inherited list; otherwise the helper
preserves both inherited NO_PROXY variants. Neither proxy URLs containing
credentials nor authentication keys are needed for this configuration.
Provider selection, models, API keys and prompt-cache settings are not changed.

Existing Claude processes keep their original environment when reattached.
The new profile takes effect only when a new Claude process starts; choose
a separate `REMOTE_TMUX_SESSION` to compare profiles without stopping work.
Both native endpoints must be updated before enabling these optional fields.
With neither field set, the old remote launch protocol is preserved.
The credential-free SSH `network-test` never routes through this API proxy.

### Validation on 2026-10-16

On the owner's Singapore server, the real SSH/tmux integration suite verified
that a deliberately nonexistent API-proxy listener did not affect SSH echo,
that only the fake Claude process received the selected proxy/bypass values,
that reattachment did not replace the live process, and that a restarted
process resumed the same UUID with its new proxy profile. No model call ran.

A fresh isolated PTY test measured timing masking enabled at 20.10 ms median
/ 22.28 ms p95 and disabled at 0.17 / 0.20 ms. This is a controlled local
network test of intentional input scheduling, not an Internet RTT reduction.
The Windows host's two company SSH aliases were inspected separately:
OpenSSH 9.5 resolved masking to `yes` before the host-specific change and
`no` afterwards. Compression remained `no`, multiplexing remained disabled,
and no ProxyCommand or ProxyJump was added. Existing connections need a new
SSH connection to pick up that configuration; they were not interrupted.

The remote interactive shells and user-level Claude settings inspected did
not expose HTTP proxy/base-URL overrides. This does not rule out project
settings or differently launched live processes. Unauthenticated direct
health GETs to api.anthropic.com returned HTTP 403 from both company hosts
(0.86/1.83 s to first response in one sample each) and HTTP 404 from Singapore
(23 ms). These are connectivity/health observations, not model first-token
latency, account validity, or a measured end-to-end proxy improvement.
Existing company proxy services and running Claude processes were not changed.

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
| `SSH_OBSCURE_KEYSTROKE_TIMING=no` | Native default: disable interactive timing masking on OpenSSH 9.5+. Set `yes` to retain it. |
| `SSH_HTTP_PROXY=http://127.0.0.1:1080` | Use a measured HTTP CONNECT route with OpenSSH socket passing. |

Except for native timing masking (default `no`), unset values inherit the user's SSH configuration. Cipher availability is
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

## Controlled interactive-input experiment

Run `sudo bash tests/network.sh` on a disposable Linux server with a C++
compiler, OpenSSH server, iproute2, network namespaces and netem support.
The test creates its own client/server namespaces, veth pair, temporary SSH
keys and daemon. Shaping is confined to those namespaces. Production eth0,
global sysctls, SSH configuration and firewall are untouched; test namespaces
and processes are removed afterwards. It can take a few minutes under load.

A C++ PTY driver sends one byte and waits for its remote echo, discarding one
warm-up and recording 20 round trips per case. Both ends use a real SSH TTY;
the remote helper puts it into raw mode. This exercises interactive input
scheduling, unlike the non-TTY `network-test`. No Claude/model calls occur.

One run on the same Singapore server (OpenSSH 10.2p1, 2026-10-15):

| Scenario | p50 input echo | p95 input echo |
| --- | ---: | ---: |
| Unshaped veth, timing masking enabled | 20.50 ms | 20.95 ms |
| Unshaped veth, timing masking disabled | 0.15 ms | 0.19 ms |
| 40 ms base RTT / 5 Mbit/s, FIFO, idle | 40.71 ms | 40.83 ms |
| Same shaped link, 64 MiB bulk transfer sharing SSH master | 3514.72 ms | 3514.86 ms |
| Same shaped FIFO, bulk and terminal on separate masters | 201.21 ms | 302.97 ms |
| Same shaped link with fq_codel below netem, separate masters | 390.17 ms | 792.33 ms |

The example config now explicitly disables timing masking and compression
for a low-latency preset (OpenSSH 9.5+). The native client now also defaults
timing masking to `no`, and uses `IgnoreUnknown` for older SSH clients.
Other unset options still inherit SSH configuration. In this local experiment, disabling masking removed about
20.35 ms of input scheduling delay. That result is separate from attachment
time and does not imply the same reduction on every Internet route.

Sharing a transport with the bulk transfer was markedly worse here. The
native client's default dedicated socket should therefore stay dedicated to
terminals; bulk transfers and model API tunnels should use other transports.
Separate transports still contend for link capacity, as the 201 ms result
shows. The fq_codel/netem arrangement performed worse in this run; it was not
applied to the live server. This is one controlled, sequential 20-sample run,
not a general congestion-control ranking or a measurement of the user's
company network.

## HTTP CONNECT without an application relay loop

Set `SSH_HTTP_PROXY=http://127.0.0.1:1080` only when that proxy is reachable
from the Linux/WSL client and performs better on the intended route. A plain
`host:port` or bracketed IPv6 `[::1]:1080` is also accepted. Authentication,
HTTPS proxy URLs and URL paths are not supported. Credentials are not read
from browser, proxy or account files.

The native bootstrap connects to the HTTP proxy, issues CONNECT, reads only
the response headers, and transfers the connected descriptor to OpenSSH via
SCM_RIGHTS (`ProxyUseFdpass=yes`). It then exits. OpenSSH owns the encrypted
transport thereafter; no Python/socat/netcat or native copying loop is added
to terminal traffic. SSH authentication, encryption and strict host-key
verification still apply. The proxy setting creates its own default master
socket profile. If a socket is explicitly overridden, disconnect first when
changing routes. Do not combine this override with a required ProxyJump.

The integration suite starts a loopback-only C++ CONNECT fixture and verifies
real authenticated SSH echo through this path, including host-key validation.

For route diagnosis, build `tests/route.cpp` as a standalone tool. Default
`target port [proxy-host proxy-port]` measures TCP connection plus SSH banner
arrival with no authentication. This does not measure steady-state RTT and
repeated unauthenticated connections can trigger SSH rate limits. `--echo`
before these arguments measures 10 byte round trips over one connection to
the matching temporary echo service. `--serve PORT` binds that echo service
to loopback for at most 120 seconds; `--serve-public PORT` explicitly binds
all IPv4 interfaces. It only echoes bounded input, with no commands or file
access, and should be used for a controlled diagnostic then stopped.

## Actual Windows/WSL to Singapore SSH results

An additional time-limited SSH daemon on the owner's Singapore server was
used to compare the real Internet paths. It used a separate temporary host
key and client public key, strict client host-key verification, and no model
calls. No production SSH config or authorized_keys file was changed. The
server/test keys and local private test key were removed after the experiment.

Each row below is a complete 50-echo authenticated `network-test` run. Routes
were also rechecked in reverse order. These are consecutive byte echoes on
an established encrypted SSH connection, excluding handshake time and tmux.

| Run / route | p50 RTT | p95 RTT | Maximum |
| --- | ---: | ---: | ---: |
| 1 / direct | 117.20 ms | 2895.14 ms | 6464.46 ms |
| 1 / HTTP proxy 1080 | 154.03 ms | 165.35 ms | 171.16 ms |
| 1 / HTTP proxy 1082 | 171.79 ms | 178.49 ms | 494.13 ms |
| 2 / HTTP proxy 1080 | 184.55 ms | 213.31 ms | 258.91 ms |
| 2 / direct | 120.76 ms | 589.03 ms | 1279.98 ms |
| 2 / HTTP proxy 1080 recheck | 167.01 ms | 188.18 ms | 192.81 ms |

The direct connection's Linux TCP counters reached 12 total retransmissions,
624 retransmitted bytes, and a congestion window of two segments in the
second run. This supports investigating transport loss/reordering/ACK delay
rather than treating all slow responses as extra propagation distance.
It does not establish an exact packet-loss percentage or which router is
responsible. ICMP/traceroute non-responses alone do not establish SSH loss.

For this host at this time, 1080 was the preferable low-jitter route: higher
median latency, but much smaller tail latency than direct. The default route
remains unchanged; select it explicitly with `SSH_HTTP_PROXY` in this target's
config and remeasure when the network or proxy node changes. The other local
proxy listener was not assumed to be interchangeable. Cold TCP/banner tests
had favored direct, demonstrating why connection-setup timings cannot replace
steady-state measurements for a long-lived terminal.
