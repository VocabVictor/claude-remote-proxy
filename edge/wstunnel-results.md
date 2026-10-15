# Cloudflare IP selection and self-hosted wstunnel

Measured on 2026-10-16 (UTC+09:00). This is a temporary transport experiment,
not a production deployment or a change to the native client's default route.

## What was actually tested

- Official CloudflareSpeedTest v2.3.5 and wstunnel v11.0.0 release binaries.
  Windows and Linux wstunnel archives matched their published SHA-256 checksums.
- 1,542 literal IPv4 candidates sampled from Cloudflare's published ranges,
  with a reproducible seed and six previously observed candidates, on each
  of Windows and two separately accessed company Linux hosts (A and B).
- HTTP HEAD screening against the owner's probe hostname, with certificate
  validation, 12 concurrent workers, two measured requests per candidate and
  downloads disabled. The probe deliberately returns 401 without credentials.
  Successful results: Windows 164, company A 472, company B 471.
- After discovering a Tokyo candidate, every address in its /24 was screened
  on each company host, with eight workers. Some edge HTTP measurements were
  near 58 ms. This is **not** the latency to the Singapore origin.
- A dedicated, temporary proxied DNS record led to a self-hosted wstunnel
  origin on Singapore TCP/80, restricted to a temporary SSH daemon on
  127.0.0.1:443. Existing zone settings were not changed. Client-to-edge WSS
  certificates and SSH host keys were verified. The HTTP origin carried the
  already encrypted SSH connection.
- The temporary SSH daemon used a generated test key, forced `/bin/cat`,
  disabled password authentication, PTYs and forwarding, and did not change
  the production sshd or root authorized_keys. No Claude/model request ran.
- Direct and relayed measurements both authenticated to that same daemon,
  discarded one warmup, then exchanged 100 sequential 32-byte SSH echoes on
  one persistent connection. These are application round trips, not TCP SYN
  timing. Python was only a temporary measurement driver, not a runtime
  dependency of the proxy or wstunnel.

The final WSS runs enabled `--websocket-mask-frame` on **both** wstunnel
endpoints. Enabling it only on the client while the origin left automatic
unmasking disabled corrupted the SSH stream. Those failed setup attempts
are excluded from the completed measurements below. A process-local
`NO_COLOR=true` was also necessary: the inherited value `1` was rejected
by this release's Boolean environment parser.

No SOCKS/HTTP listener, system proxy or WARP was started on a client.
Proxy environment variables were removed from the test subprocesses.

## Completed 100-echo measurements

Sequential WAN observations, not simultaneous trials or long-term guarantees.
Edge labels were additionally checked against the actual tunnel hostname:
the local candidate reported HKG and the company candidate reported NRT.

| Client | Route | First response ms | Median ms | p95 ms | Maximum ms |
| --- | --- | ---: | ---: | ---: | ---: |
| Windows | Direct SSH | 6429.93 | 424.72 | 431.57 | 1109.63 |
| Windows | CF 162.159.132.27 | 3164.05 | 246.04 | 352.22 | 675.36 |
| Windows | CF 162.159.129.1 | 3921.45 | 222.99 | 228.74 | 610.90 |
| Company A | Direct SSH | 2106.94 | 125.51 | 1476.87 | 2500.98 |
| Company A | CF 172.64.229.185 | 4108.70 | 278.78 | 306.71 | 649.07 |
| Company A | CF 172.64.229.90 | 4281.75 | 297.78 | 315.44 | 708.84 |
| Company A repeat | Direct SSH | 2582.78 | 127.28 | 837.68 | 1868.36 |
| Company A repeat | CF 172.64.229.19 | 1940.45 | 140.58 | 172.26 | 422.55 |
| Company B | Direct SSH | 4190.11 | 120.46 | 469.71 | 1277.73 |
| Company B | CF 172.64.229.185 | 3467.97 | 275.95 | 295.25 | 1342.96 |
| Company B | CF 172.64.229.90 | 2016.60 | 140.96 | 146.84 | 446.38 |

The selected Windows route improved the median from about 425 to 223 ms.
The selected company A repeat increased the median from 127 to 141 ms but
reduced p95 from 838 to 172 ms. Company B's selected route increased the
median from 120 to 141 ms but reduced p95 from 470 to 147 ms. The company
benefit is fewer long stalls, not a lower minimum propagation delay.
IPs in the same /24 were not interchangeable in these runs.
An earlier 50-echo run through 172.64.229.19 measured company A at 140.52 ms
median / 147.42 ms p95, while its paired direct run was 130.43 / 1401.28 ms.
Do not replace a working direct route based only on a two-request HTTP score.

## Separate direct QUIC experiment

wstunnel WebTransport on temporary UDP/8443 also completed 50 SSH echoes:

| Client | Median ms | p95 ms | Maximum ms |
| --- | ---: | ---: | ---: |
| Windows | 431.11 | 440.17 | 1356.11 |
| Company A | 120.16 | 1294.98 | 3567.20 |
| Company B | 120.43 | 1440.28 | 1774.53 |

This did not establish a consistent advantage over direct SSH. The outer
QUIC test used a generated, self-signed certificate without CA verification;
the inner SSH host key remained pinned to the key independently obtained
through the cloud management API. This is not a production TLS configuration.
An initial one-second connection budget was too short; a 12-second budget
allowed the test to complete. UDP reachability alone did not remove WAN stalls.

## Optional native SSH profile

The native client already accepts a dedicated SSH configuration through
`CLAUDE_PROXY_SSH_CONFIG`; no new native forwarding loop or Python is needed.
This example is a template, not an installed service or a live endpoint.
Use your own domain, selected edge IP, login and existing trusted SSH host key.

On an otherwise unused origin TLS port, configure a valid origin certificate
and restrict the server to the local SSH endpoint:

```sh
wstunnel server --websocket-mask-frame --restrict-to 127.0.0.1:22 \
  --tls-certificate /secure/origin.crt --tls-private-key /secure/origin.key \
  wss://0.0.0.0:443
```

Configure the dedicated proxied hostname to reach that origin using validated
origin TLS. Do not change a shared zone's SSL mode merely to try this template.
The client-side profile is:

```sshconfig
Host sg-edge
    HostName YOUR_SINGAPORE_SERVER
    User YOUR_LOGIN
    Port 22
    StrictHostKeyChecking yes
    ProxyUseFdpass no
    ProxyCommand env -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY -u http_proxy -u https_proxy -u all_proxy NO_COLOR=true wstunnel client --log-lvl off --tls-verify-certificate --tls-sni-override tunnel.example.com --websocket-mask-frame -H 'Host: tunnel.example.com' -L stdio://127.0.0.1:%p wss://YOUR_SELECTED_EDGE_IP
```

Select this file with `CLAUDE_PROXY_SSH_CONFIG=/absolute/ssh-cloudflare.conf`
and `SSH_TARGET=sg-edge`; leave `SSH_HTTP_PROXY` unset. A separate config
filename keeps the native client's route-specific SSH master separate from
the direct profile. Keep the direct profile available. Remeasure the complete
SSH path before changing defaults, especially after IP or ISP route changes.

## References

Cleanup was verified after testing: both company-host test private keys and
the local test private key were removed; Singapore's temporary TCP/80,
TCP/443 and UDP/8443 listeners were stopped, and its test authentication and
private host/TLS keys removed. The newly created DNS record and TCP/443 /
UDP/8443 firewall rules were removed. Original TCP/22, TCP/80 and ICMP rules
were preserved. The production SSH daemon and the probe-only Worker remain
unchanged. No production CC entry point was switched.

- [CloudflareSpeedTest](https://github.com/XIU2/CloudflareSpeedTest)
- [wstunnel](https://github.com/erebe/wstunnel)
- [wstunnel WebSocket masking implementation](https://github.com/erebe/wstunnel/blob/main/wstunnel/src/tunnel/transport/websocket.rs)
- [Official Cloudflare IPv4 ranges](https://www.cloudflare.com/ips-v4)
