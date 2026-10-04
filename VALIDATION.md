# Validation for configuration, cache, CI, and HTTPS

## Small generated response optimization

- Small generated response bodies now fit beside their headers in the existing connection output buffer. This avoids a separate body write while retaining stable buffers for partial HTTP/TLS writes. Body-byte accounting excludes headers and HEAD bodies; shutdown header edits also adjust the body boundary.
- `strace` counted 200 socket writes before and 100 after for 100 successful `/api/status` requests. This reduction applies to bodies that fit with their headers in the 1,536-byte buffer; larger bodies retain the existing chunk path.
- Six alternating trials per build, 5,000 requests per trial, 16 concurrent keep-alive clients, 20 warmup requests, request cap 10,000, and access logging disabled: all 60,000 measured requests succeeded. Median throughput changed from 10,323 to 10,670 requests/s (+3.4%). This small loopback difference may include run-to-run variation; the reliable measured benefit is fewer socket writes.
- Strict build, all 50 HTTP/HTTPS integration tests, cache unit tests, and both precompression-tool tests passed. Access logging checks verify exact body-byte counts for a combined health response and zero body bytes for HEAD.
- An isolated HTTP server with a temporary `LD_PRELOAD` send wrapper capped every successful write at seven bytes and returned EAGAIN every third call. GET, HEAD, small echo, larger echo, and API error responses matched their expected bytes; access logs recorded exact body lengths and completion for all five requests.

## Phase 5: routing and JSON APIs

- Method/path dispatch now covers health, metrics, API status, and JSON echo before static fallback. API errors use JSON with stable codes and route-specific Allow headers.
- All 50 HTTP/HTTPS integration tests passed under AddressSanitizer and UndefinedBehaviorSanitizer. Cache unit tests and both precompression-tool tests also passed. New checks cover route selection, media types, UTF-8/JSON validation, the exact 16 KiB body boundary, fragmented bodies, mixed POST/static/API pipelines, framing rejections, body timeouts, partial-body disconnects, concurrent health requests, and shutdown cleanup.
- Deterministic parser/range/JSON fuzzing passed 100,000 mutations, including a sanitizer-instrumented run. Strict TLS-enabled and HTTP-only builds passed.
- Bodies are buffered only for accepted echo requests, bounded by Content-Length and a fixed 16 KiB limit. Chunked transfer and Expect remain unsupported. JSON nesting is limited to 32 containers; echo preserves input bytes rather than constructing a parsed object tree.

## Phase 4: precompressed gzip assets

- Strict TLS-enabled build, all 44 HTTP/HTTPS integration tests, cache unit tests, and two asset-tool tests passed. Coverage includes negotiation, validators, HEAD, ranges, mixed pipelines, cache variants, expiry, sidecar updates, missing/stale/unsafe sidecars, and verified HTTPS.
- Parser/range fuzzing passed 100,000 mutations with encoding quality invariants.
- AddressSanitizer and UndefinedBehaviorSanitizer checks passed for 100,000 parser/range mutations and cache unit tests, with local leak detection disabled. The strict HTTP-only build also passed.
- A synthetic repeated-text asset shrank from 64,000 bytes to a 280-byte gzip sidecar (99.56%). With cache enabled, concurrency 16, three trials of 2,000 requests per representation, and logging disabled, all 12,000 requests succeeded. Median throughput was 8,400 requests/s for identity and 8,552 for gzip. This small throughput difference does not establish a speed improvement; the payload reduction depends heavily on this repetitive fixture.
- Runtime sidecar validation checks file type, freshness, and magic bytes. Full decompression and content verification happen in the offline tool; deployments must publish coherent originals and sidecars.

- GCC strict C11 build with warnings treated as errors passed.
- TLS-enabled integration suite: 38 tests, 37 passed and one optional process-descriptor check skipped because this runtime does not expose the descriptor table.
- AddressSanitizer and UndefinedBehaviorSanitizer: same integration suite passed; the cache ownership unit test also passed with instrumentation. Leak detection was disabled locally because this runtime lacks the required process access. CI enables leak detection on Ubuntu.
- HTTP-only strict build: 38 tests, 34 passed, three HTTPS tests and the optional descriptor check skipped.
- Cache unit test covers pinned data, capacity, expiry retirement, release, and ownership.
- Deterministic parser/range fuzzing: 100,000 mutations passed.
- Verified HTTPS benchmark and stress smoke runs: 100 requests each, zero errors. These are functional checks, not throughput baselines.
- GitHub Actions workflow supplied; remote CI and Clang coverage-guided fuzzing have not been run in this session.

## Phase 2: hashed file cache and LRU list

- Strict C11 build and all 38 server integration tests passed, including cache, ranges, conditional requests, pipelines, HTTPS, and disconnects.
- Expanded cache unit checks passed for many paths, LRU order, pinned eviction, expiry, replacement, capacity, and caller ownership after bypass.
- Loopback benchmark: 512 files, 10,000 requests per trial, 16 concurrent keep-alive clients, 512 warmup requests, three trials, cache TTL 60 seconds, logging disabled. Median successful throughput changed from 8,881 to 8,973 requests/s (+1.0%), with zero errors. This small difference is within likely run-to-run variation and is not evidence of a server throughput gain.
- Direct warm cache lookup benchmark: 300,000 lookups across 3,000 entries took 5.934 and 6.142 seconds with the previous list lookup, versus 0.034 and 0.036 seconds with the hash table. This measures lookup cost alone, not end-to-end HTTP capacity.

### Extended local checks

- Temporary loopback server, 512 files of about 130 bytes, cache limit 16 MiB, entry limit 1,024, TTL 60 seconds, access logging disabled. Three trials of 10,000 requests at concurrency 16 produced median throughput of 8,376 requests/s with cache off and 8,362 requests/s with cache on; all 60,000 requests succeeded. The difference is within run-to-run variation.
- Fresh-connection benchmark against one cached file: three trials of 3,000 requests at concurrency 16, median 5,491 requests/s, zero errors.
- Stress run held 100 idle connections and 8 slow readers of a 16 MiB file while completing 2,000 fresh `/health` requests at concurrency 32 with zero request errors. After the held sockets closed, metrics showed 512 cache entries using 1,357,312 bytes, below the configured limits. Eight aborted responses corresponded to the slow readers closing before their transfers finished.
- Deterministic parser/range fuzz run passed 100,000 mutations.

## Pre-Phase 3 cache memory cleanup

- Cache entries now allocate only the path length they use and retain response metadata without unused file descriptor and `stat` fields. For the same 512-file working set, `c_http_cache_bytes` fell from 1,357,312 to 238,592 bytes (82.4% less counted cache memory). The hash table and allocator overhead remain outside that gauge.
- An experimental direct-write path for cached responses did not show a reliable HTTP throughput benefit, so it was removed. The final code retains the existing response-write path.
- Alternating old/new runs for a cached 32 KiB file, 5,000 requests per trial, concurrency 16, six trials per version: median successful throughput was 8,837 requests/s for the previous code and 8,614 requests/s for the optimized code, with high trial variance and zero errors. This benchmark does not establish a throughput improvement; the measured benefit is cache memory use.
- Strict C11 build, cache unit tests, and all 39 HTTP/HTTPS integration tests passed. The cache unit test also passed with AddressSanitizer and UndefinedBehaviorSanitizer locally, with leak detection disabled in this runtime.

## Phase 3: large-file transfers

- Plain HTTP, uncached regular files now use `sendfile()` to nonblocking client sockets in bounded 64 KiB calls. HTTPS and unsupported `sendfile()` cases use the existing asynchronous read path. A source page-cache miss can still stall the reactor on slow storage; this path does not make disk I/O asynchronous.
- A local 1 MiB-file benchmark with cache disabled, concurrency 8, and three trials of 100 requests measured median successful throughput of 2,350 requests/s before and 4,849 requests/s after (+106%); all 600 requests succeeded. This is a loopback workload, not a general capacity claim.
- An uncached full-file and cross-chunk range test verified exact response bytes and that each plain HTTP response submitted only its file-open job. Existing slow-reader, disconnect, and HTTPS tests passed; HTTPS submitted disk read jobs as expected.
- A local `LD_PRELOAD` test made `sendfile64()` return a successful partial transfer, then `EINVAL`. The remaining 887,656-byte range body matched the source exactly, and the server resumed through 27 worker read jobs after its file-open job.
- A stress run held 100 idle sockets and 16 slow readers of an 8 MiB file while completing 5,000 health requests at concurrency 32 with zero errors. The server submitted 16 file-open jobs and no body-read jobs; 16 aborted responses were the slow readers closed by the test.
- The strict TLS-enabled build and all 40 integration tests passed. A strict HTTP-only build also passed.
