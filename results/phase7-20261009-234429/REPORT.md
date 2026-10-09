# Phase 7 local benchmarks

Optimized TLS-capable build, plain HTTP over IPv4 loopback, access logging disabled, 16 MiB cache, 1-second cache TTL. Three trials per workload, 20 warmup requests per trial, keep-alive enabled. Small workloads use 16 clients and 3,000 requests per trial; the 16 MiB file uses four clients and 40 requests per trial. Values below are medians across trials.

| Workload | Requests/s | p50 ms | p95 ms | p99 ms | Errors |
| --- | ---: | ---: | ---: | ---: | ---: |
| ready | 8,833 | 1.161 | 5.094 | 8.057 | 0 |
| status | 9,483 | 0.957 | 4.928 | 7.840 | 0 |
| cached-css | 8,078 | 1.521 | 4.674 | 6.588 | 0 |
| gzip-css | 7,460 | 1.715 | 4.865 | 6.885 | 0 |
| echo | 8,059 | 1.559 | 4.747 | 6.704 | 0 |
| large-file | 190 | 16.478 | 38.294 | 40.879 | 0 |

Stress: 300 idle connections and eight slow readers of the 16 MiB file, with 5,000 fresh-connection health requests from 32 clients. All requests succeeded: 5,712 requests/s, p95 10.523 ms, p99 14.099 ms. The server remained ready afterward (HTTP 200). Idle connections have a five-second timeout; the measured stress request phase lasted 0.8754 seconds.

Total: 50,120 measured successful requests, excluding warmups. The large-file median corresponds to roughly 3,044 MiB/s of application response payload. This is local transfer throughput with cached OS file pages, not disk throughput.

These are exploratory client-and-server loopback results, not a production capacity claim or a before/after regression comparison. Python client scheduling, shared host load, small sample sizes, and sequential workload order affect results. Gzip uses existing precompressed assets; the difference from CSS does not isolate compression cost. No HTTPS, authentication, remote network, or journal logging benchmark was run.

Raw JSON files contain trial details and environment information; metrics.txt is the server snapshot after stress. The isolated server was stopped, and temporary assets removed. The saved server.conf records the tested inputs but its temporary document_root no longer exists; replace it with public to run it again (and create a 16 MiB large.bin in a separate test document root for the file workload). Existing cache-on/off results were preserved.
