# C Reverse Proxy and Load Balancer Roadmap

## Product goal

Evolve our existing C HTTP server into a reverse proxy and load balancer with HTTPS, bounded resource use, authentication, static-file serving, operational metrics, and deployment support. Each phase must leave the project usable and tested.

The first release targets HTTP/1.1 on Linux, building on the existing epoll reactor and connection state machine.

## Existing foundation

The project already includes:

- An epoll reactor and connection FSM.
- HTTP/1.1 keep-alive and pipelining.
- Static-file serving, HEAD, conditional requests, and single byte ranges.
- Bounded asynchronous filesystem work and background access logging.
- A bounded small-file content cache with TTL expiry.
- Optional nonblocking HTTPS through OpenSSL.
- Configuration files, Prometheus metrics, and graceful shutdown.
- Integration tests, parser/range fuzzing, benchmark and stress tools.
- A GitHub Actions workflow prepared for builds, tests, sanitizers, and fuzzing.

These are the starting point; the phases below are planned enhancements, not a claim that the final proxy is already implemented.

## Implementation roadmap

| Phase | Features to implement | Completion criteria |
| --- | --- | --- |
| 1. Benchmark tooling | Repeated trials, comparison of saved results, cache on/off runs, median throughput and latency summaries, environment/config recording | Compare two configurations reproducibly and identify errors or regressions |
| 2. Cache improvements | Hash-table lookup, LRU eviction list, existing byte/entry limits and pinned-response ownership | Improve many-file workloads without breaking expiry, ranges, or memory bounds |
| 3. Large-file transfers | `sendfile()` for eligible uncached plain HTTP responses; partial-transfer handling and fallback | Large files and ranges work under slow readers, disconnects, and backpressure |
| 4. Compression | Precompressed gzip assets, `Accept-Encoding` negotiation, `Vary`, representation-specific validators, explicit range behavior | Clients receive the correct representation; caching and conditional requests remain correct |
| 5. Routing and JSON APIs | Method/path router, handler interface, JSON responses, bounded request-body parsing, consistent API errors | Static files and API routes coexist; malformed and oversized requests are handled safely |
| 6. Authentication and cryptography | Secure random API tokens, protected routes, HMAC-SHA-256 request signing, timestamp validation, bounded nonce replay tracking, key rotation | Reject invalid credentials, modified requests, expired signatures, and replay attempts |
| 7. Deployment support | Configurable bind address, readiness endpoint, systemd service, example deployment configuration, startup validation | Run under a dedicated account and report readiness correctly during startup and draining |
| 8. Single-backend reverse proxy | Upstream connection FSM, nonblocking connect, request forwarding, streamed responses, bounded buffers, correct HTTP framing and hop-by-hop header handling | Forward requests reliably to one backend without whole-response buffering |
| 9. Upstream pooling and HTTPS | Reusable upstream connections, pool limits, connect/read/write deadlines, verified upstream TLS, configurable trust and server name | Reuse backend connections safely and handle certificate verification failures correctly |
| 10. Load balancing | Backend groups, round-robin, weighted round-robin, least-connections, active health checks, passive failure detection | Distribute traffic predictably and remove unhealthy backends from rotation |
| 11. Resilience and traffic controls | Per-backend concurrency limits, bounded admission queues, rate limiting, circuit breakers, restricted retries, backend draining | Prevent slow or failed backends from exhausting the proxy; produce controlled overload responses |
| 12. Operations and release hardening | Per-backend metrics, request IDs, proxy access logs, validated configuration reload, backend membership updates, expanded fuzzing and failure tests | Preserve active traffic during reloads, make failures observable, and pass repeatable release validation |

## Security and cryptography

### Client HTTPS

Extend the existing TLS support as the proxy evolves. Keep TLS handshakes and transport I/O integrated with the nonblocking reactor.

### Upstream HTTPS

Encrypt proxy-to-backend connections and verify backend certificate chains and hostnames. Configure trust and the expected server name explicitly.

### API tokens

Generate tokens with a cryptographically secure random source. Protect designated routes, avoid logging credentials, and support revocation and rotation.

### HMAC-SHA-256 request signing

Define exactly which method, request target, selected headers, and body bytes are signed. Specify canonicalization before implementing clients. Validate timestamps and track nonces within a bounded replay-protection window. Use constant-time verification where appropriate.

### File integrity

Optionally expose SHA-256 digests for static assets, with trusted digest distribution. A plain digest verifies consistency against an expected value; by itself it does not authenticate the source.

### Key management

Keep keys and secrets outside the document root, restrict filesystem permissions, avoid including secrets in logs or source control, and support rotation. Use established cryptographic libraries rather than implementing cryptographic primitives ourselves.

### Encrypted storage: conditional extension

Add AES-GCM encrypted storage only when the product introduces persisted sensitive data. Before implementation, define the storage format, authenticated metadata, nonce management, key lifecycle, and crash-recovery behavior.

This is a later extension, not a prerequisite for the first proxy release.

## Final product behavior

The finished service will:

- Accept HTTP/1.1 client traffic, including HTTPS.
- Route requests to static handlers, JSON handlers, or backend groups.
- Authenticate protected routes and verify signed requests where configured.
- Select healthy backends using the configured balancing policy.
- Reuse upstream connections within bounded pools.
- Stream traffic with backpressure and bounded buffers.
- Enforce connection, queue, timeout, and traffic limits.
- Serve static assets with caching, compression, ranges, and efficient large-file transfers.
- Expose operational metrics and structured access logs.
- Support readiness checks, graceful draining, and validated configuration reloads.

## Validation strategy

Every phase receives tests appropriate to its behavior and risk. Performance changes are compared against the same controlled baseline.

### Performance comparisons

Record workload, connection reuse, concurrency, request counts, configuration, logging settings, software versions, and relevant host details. Run repeated trials and report errors alongside throughput and latency. Distinguish functional smoke tests from capacity measurements.

### Correctness and resource handling

Exercise partial reads and writes, fragmented requests, pipelines, queue saturation, cache ownership, disconnect cleanup, expiry, ranges, and shutdown. Check that configured resource bounds hold under stress.

### Proxy failure scenarios

Test backend crashes, malformed responses, stalled connects and transfers, client disconnects, unavailable backend groups, and pool exhaustion. Define retry eligibility carefully so failures do not cause unsafe duplicate work. Account for whether request or response bytes have already been forwarded.

### Security testing

Test incorrect and revoked tokens, altered signed requests, expired timestamps, repeated nonces, invalid certificates, hostname mismatches, and accidental credential exposure in logs.

### Release checks

Run strict compiler builds, integration tests, sanitizers, parser and protocol fuzzing, stress scenarios, deployment checks, and configuration-reload tests. Document known limitations and reproducible benchmark conditions.

## Later extensions

These are separate extensions after the first HTTP/1.1 release:

- HTTP/2 and HTTP/3.
- WebSocket tunneling.
- Distributed rate limiting.
- Multi-node coordination.
- Encrypted persistent storage when a concrete sensitive-data use case exists.

## Next step

Implement **Phase 1: the benchmark comparison tool** before beginning further performance changes.
