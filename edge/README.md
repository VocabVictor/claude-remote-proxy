# Cloudflare edge experiment (not an enabled SSH transport)

**2026-10-16 follow-up:** broader IP selection plus a self-hosted native
wstunnel origin completed authenticated, end-to-end SSH echo tests without
a local proxy listener. Some paths improved substantially; results vary by
client and IP. See [measurements and an optional SSH profile](wstunnel-results.md).
The sections below describe the initial Worker-only experiment, not a limit
on all Cloudflare routes. The Worker itself remains probe-only.

The owner's Cloudflare Free account was tested on 2026-10-15 with Wrangler
4.108.0. A new authenticated probe-only Worker is deployed at
`https://ccproxy-edge.34920208.xyz`. No paid plan, Spectrum, Argo, WARP,
system proxy or localhost proxy listener was enabled.

The ordinary `workers.dev` hostname resolved to non-Cloudflare addresses on
this network and TLS was reset even with verified DNS addresses. An unused
subdomain in the owner's active zone was therefore bound to this Worker.
Existing records/services were not replaced. The selected free edge reported
**LAX**, rather than a nearby Asian ingress.

Direct C++/libcurl WSS probe, 10 measured echoes after a discarded warm-up:

| Metric | Result |
| --- | ---: |
| Edge location | LAX |
| Median | 310.20 ms |
| p95 / maximum | 4861.41 ms |

A separate Node probe measured a 967.34 ms median and 3147.50 ms maximum in
another 10-sample run. Longer attempts also timed out. These are small,
sequential observations at different times, not a controlled ranking of Node
versus C++, and not end-to-end SSH latency results.

A temporary WSS-to-SSH experiment reached public-key authentication on the
test server, but did not complete the 50-echo SSH test. It is not a validated
production transport. The forwarding source and client integration were
removed. The final Worker only performs bounded binary echo at `/probe`;
it does not connect to the server or forward arbitrary traffic. The temporary
SSH daemon and its private test keys were removed. An ignored experimental
binary may remain locally, but the production client does not invoke it.

**Decision: do not switch the terminal to Cloudflare automatically.** Even
the edge-only path was slower and unstable during this experiment. The
existing native SSH runtime is unchanged. Cloudflare's automatic edge
selection does not guarantee lower latency on a particular ISP route.

## Repeat the direct test

The credential is generated locally, stored under the Git-ignored `.secrets`
directory with user/System-only Windows ACLs, and installed as the Worker's
`EDGE_AUTH_TOKEN` secret. Never put it in a URL, command argument, source file
or log. The Worker returns 401 without authentication; authenticated paths
other than `/probe` return 404. Per-session message/frame/byte limits apply.
The final deployment was checked directly with hostname/certificate validation:
unauthenticated `/probe` returned 401, authenticated `/ssh` returned 404, and
the C++ binary echo completed. A later three-sample health recheck still
reported LAX with a 1016.79 ms median, reinforcing the decision not to switch.

Build the optional diagnostic (libcurl with WebSocket support is required):

```sh
c++ -O2 -std=c++17 tests/edge_probe.cpp -lcurl -o /tmp/ccproxy-edge-probe
/tmp/ccproxy-edge-probe --probe wss://ccproxy-edge.34920208.xyz/probe .secrets/edge.token - 10
```

An optional literal IP can replace `-` for a bounded comparison using the
same hostname/SNI and full certificate verification. The C++ diagnostic
explicitly disables HTTP/SOCKS proxy environment settings. It adds no
dependency or startup overhead to the production native client.

The Node comparison is `node tests/edge-probe.mjs URL TOKEN_FILE 10`; clear
proxy environment variables in that process before using it for a direct
comparison. The C++ probe is the authoritative proxy-disabled test.

Wrangler configuration disables telemetry and Worker observability. Unbounded
CLI telemetry waits were observed after successful deployments on this
network; `send_metrics = false` avoids that management-side delay. Workers
Free defaults apply; no CPU override or paid feature is configured.

Official references:

- [Spectrum is a paid-plan service](https://developers.cloudflare.com/spectrum/).
- [WebSockets are supported on all plans](https://developers.cloudflare.com/network/websockets/).
- [Workers limits and interruptions](https://developers.cloudflare.com/workers/platform/limits/).
- [Mainland China network requires Enterprise plus a separate subscription](https://developers.cloudflare.com/china-network/).
- [Workers binary message compatibility change](https://developers.cloudflare.com/workers/runtime-apis/websockets/).
