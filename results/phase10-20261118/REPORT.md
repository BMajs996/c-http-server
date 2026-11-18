# Phase 10 load balancing benchmark

Three identical C HTTP backends ran on IPv4 loopback. The proxy served
`/proxy/api/status` using a shared pool budget of 64 connections and an idle
limit of 16. Logging and active probes were disabled. Backend weights were
5:1:1. Each policy ran three trials with 16 concurrent keep-alive clients,
3,000 measured requests and 20 warmup requests per trial. Each trial used a
fresh proxy process.

| Policy | Median requests/s | Median p95 (ms) | Assignments to a / b / c per trial |
| --- | ---: | ---: | --- |
| Round-robin | 8,261 | 4.126 | 1,007 / 1,007 / 1,006 |
| Smooth weighted round-robin | 8,396 | 4.050 | 2,157 / 432 / 431 |
| Least-connections | 7,785 | 4.609 | 1,003–1,008 / 1,005–1,008 / 1,005–1,012 |

All 27,000 measured requests and 180 warmups succeeded. Assignment counts
include warmup. The weighted distribution matched 5:1:1 within one request
of each expected count. The least-connections workload had identical, fast
backends, so assignments were close to equal. A separate integration test
holds one slow request open and checks that the other member gets new
requests until the slow lease ends.

These short local measurements include the Python client and three backend
processes on the same machine. They establish the observed distribution
under this workload. Throughput differences need repeated measurements
under representative load before using them to choose a policy.

To reproduce, build with `-O2` and adapt
[server-balanced.conf](../../server-balanced.conf) for three C backends.
Set `access_log=0` and `max_requests=10000` on all processes, set
`upstream_health_enabled=0` on the proxy and use the same weights. Start a
fresh proxy for each trial and repeat for each policy.

```sh
python3 tools/benchmark.py --path /proxy/api/status --requests 3000 \
  --concurrency 16 --warmup 20 --trials 1
curl -fsS http://127.0.0.1:8080/metrics | grep '^c_http_backend_requests_total'
```

Full trials and environment details are in [benchmark.json](benchmark.json).
