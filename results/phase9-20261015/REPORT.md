# Phase 9: local upstream pool comparison

Optimized TLS=1 build, IPv4 loopback, client HTTP keep-alive, one persistent
Python backend fixture and one separate C proxy process. The backend uses
TCP_NODELAY and returns a small JSON response. HTTPS verifies the generated
backend CA and DNS SAN; mTLS additionally verifies a client certificate signed
by a separate CA. Access logging and file caching are disabled.

Each of the six modes ran three trials of 1,500 measured requests at concurrency
16, with 20 warmup requests per trial and a new proxy process for each trial.

| Backend mode | Pool | Median requests/s | Median p95 ms | Completed | Errors |
| --- | --- | ---: | ---: | ---: | ---: |
| HTTP | off | 2,611.59 | 7.858 | 4,500 | 0 |
| HTTP | on | 4,315.41 | 6.431 | 4,500 | 0 |
| HTTPS | off | 1,350.29 | 15.110 | 4,500 | 0 |
| HTTPS | on | 3,887.40 | 7.071 | 4,500 | 0 |
| mTLS | off | 703.33 | 27.294 | 4,500 | 0 |
| mTLS | on | 3,522.79 | 7.791 | 4,500 | 0 |

All **27,000 measured requests** and 360 warmups succeeded. The observed
throughput ratios were about **1.65× HTTP, 2.88× HTTPS, and 5.01× mTLS** with
pooling enabled in this fixture.

## Connection and TLS work

In every trial, including warmup:

| Pool | Opened sockets | Reused leases | TLS handshakes (HTTPS/mTLS) |
| --- | ---: | ---: | ---: |
| off | 1,520 | 0 | 1,520 |
| on | 16 | 1,504 | 16 |

The pool removed about 98.95% of fresh socket opens and TLS handshakes in this
workload. These are retained live sessions, without an application retry or a
TLS handshake on each lease. Samples after each trial showed 16 idle sockets
and no leased/closing sockets with pooling on, and zero upstream sockets with
pooling off. Proxy descriptor samples were 23 versus 7, matching 16 retained
backend sockets. RSS samples ranged from 8,296 to 12,960 KiB across all modes;
they are process snapshots, not peaks, and exclude kernel socket buffers.

## Limits of the measurements

These results include Python client/backend overhead, TLS work at both peers,
thread creation for fresh backend connections, and shared-host scheduling.
The backend and benchmark client share a Python process. Workload modes ran
sequentially. They demonstrate connection reuse and its effect on this fixture;
they do not isolate C proxy CPU cost or establish production capacity. They
are not comparable to Phase 8's separate C-backend benchmarks.

No frontend TLS, multi-backend routing, production certificates, or WAN latency
is represented. mTLS has a dedicated client CA; server and client trust are
separate. Test keys and certificates existed only in temporary directories and
were removed after the run. JSON response identity numbers change in length
between connections, so payload sizes vary slightly.

## Reproduce

From the repository root, with Python 3, OpenSSL 3 development libraries, the
openssl CLI, and local socket access:

```sh
make clean
make CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O2'
python3 results/phase9-20261015/run.py /tmp/phase9-repeat
```

The driver uses the controlled integration-test backend, generates temporary
certificates, launches isolated proxy processes, and stops them afterward.
Raw JSON records every trial, latency percentiles, pool/TLS metrics, process
snapshots, core configuration, compiler/runtime versions, and measured binary
SHA-256. Pool defaults are 64 total, 16 idle, 15-second idle expiry, five-minute
age, and 1,000 requests per socket. The proxy total deadline is 30 seconds.
