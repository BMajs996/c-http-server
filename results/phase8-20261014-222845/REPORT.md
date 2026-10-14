# Phase 8 local proxy benchmarks

Optimized TLS-capable build, plain HTTP over IPv4 loopback, cache/access logging disabled. Backend and proxy are separate instances of the C server. Three trials per workload with 20 warmup requests per trial. Status uses 3,000 requests and 16 clients per trial; large transfers use 40 requests and four clients per trial, each response 8 MiB.

| Workload | Median requests/s | Median p95 ms | Completed | Errors |
| --- | ---: | ---: | ---: | ---: |
| status-direct | 9,189.69 | 4.868 | 9000 | 0 |
| status-proxy | 7,707.26 | 4.438 | 9000 | 0 |
| large-direct | 334.04 | 20.490 | 120 | 0 |
| large-proxy | 217.23 | 23.564 | 120 | 0 |

## Slow readers and memory

Sixteen clients opened large responses without consuming their bodies, with 4 KiB receive buffers. While those readers were held, 2,000 additional proxy status requests ran at concurrency 16 in each case. Both stress workloads had zero errors.

| Response size | Active upstreams at sample | Proxy RSS KiB |
| --- | ---: | ---: |
| 8 MiB | 16 | 9612 |
| 64 MiB | 16 | 9576 |

The RSS snapshots stayed around 9.4 MiB despite increasing response size eightfold. These are two process-memory samples, not peak measurements, and exclude kernel socket buffers and system page cache. Bounded application buffers are also enforced by the implementation.

## Interpretation and reproduction

All 22,240 measured requests completed successfully, excluding warmups. Direct clients reuse backend connections; proxy clients reuse frontend connections but the proxy opens a fresh backend connection for each request. This is intentional Phase 8 behavior; pooling is planned for Phase 9. Large direct plain-HTTP transfers use sendfile, while the proxy copies through its streaming buffers.

Results include Python client overhead, shared-host load, and sequential workload order. They are exploratory loopback measurements, not production capacity or isolated CPU-cost claims. Smaller proxy p95 values for status should not be interpreted as a reliable latency improvement. No upstream TLS, pooling, authentication, or journal logging benchmark was run.

Raw JSON files record trials, environment, and configuration hashes. Configurations use temporary document roots, removed after testing; substitute an existing test root before rerunning. Generate large.bin as bytes(range(256)) repeated 32,768 times (8 MiB), and huge.bin with the same pattern repeated 262,144 times (64 MiB). Keep cache and logging disabled. Run tools/benchmark.py with /api/status or /large.bin on the backend, and /proxy/api/status or /proxy/large.bin on the proxy, using the trial settings above. Both isolated server processes were stopped after the run.
