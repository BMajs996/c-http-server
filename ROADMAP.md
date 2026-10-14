# C Reverse Proxy and Load Balancer Roadmap

## Product goal

Evolve our existing C HTTP server into a reverse proxy and load balancer with
HTTPS, bounded resource use, authentication, static-file serving, operational
metrics, and deployment support. Each phase must leave the project usable and
tested.

The first release targets HTTP/1.1 on Linux, building on the existing epoll
reactor and connection state machine.

After the Phase 12 release, extend the service into a C API gateway with
backend response caching and real-time forwarding. Phases 13–17 continue the
focus on sockets, protocol handling, explicit memory ownership, bounded
resources, and measurable performance.

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

These are the starting point; the phases below are planned enhancements, not a
claim that the final proxy is already implemented.

## Implementation roadmap


|Phase                                  |Features to implement                                                                                                                                     |Completion criteria                                                                                                    |
|---------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------|
|1\. Benchmark tooling                  |Repeated trials, comparison of saved results, cache on/off runs, median throughput and latency summaries, environment/config recording                    |Compare two configurations reproducibly and identify errors or regressions                                             |
|2\. Cache improvements                 |Hash-table lookup, LRU eviction list, existing byte/entry limits and pinned-response ownership                                                            |Improve many-file workloads without breaking expiry, ranges, or memory bounds                                          |
|3\. Large-file transfers               |`sendfile()` for eligible uncached plain HTTP responses; partial-transfer handling and fallback                                                           |Large files and ranges work under slow readers, disconnects, and backpressure                                          |
|4\. Compression                        |Precompressed gzip assets, `Accept-Encoding` negotiation, `Vary`, representation-specific validators, explicit range behavior                             |Clients receive the correct representation; caching and conditional requests remain correct                            |
|5\. Routing and JSON APIs              |Method/path router, handler interface, JSON responses, bounded request-body parsing, consistent API errors                                                |Static files and API routes coexist; malformed and oversized requests are handled safely                               |
|6\. Authentication and cryptography    |Secure random API tokens, protected routes, HMAC-SHA-256 request signing, timestamp validation, bounded nonce replay tracking, key rotation               |Reject invalid credentials, modified requests, expired signatures, and replay attempts                                 |
|7\. Deployment support                 |Configurable bind address, readiness endpoint, systemd service, example deployment configuration, startup validation                                      |Run under a dedicated account and report readiness correctly during startup and draining                               |
|8\. Single-backend reverse proxy       |Upstream connection FSM, nonblocking connect, request forwarding, streamed responses, bounded buffers, correct HTTP framing and hop-by-hop header handling|Forward requests reliably to one backend without whole-response buffering                                              |
|9\. Upstream pooling and HTTPS         |Reusable upstream connections, pool limits, connect/read/write deadlines, verified upstream TLS, configurable trust and server name                       |Reuse backend connections safely and handle certificate verification failures correctly                                |
|10\. Load balancing                    |Backend groups, round-robin, weighted round-robin, least-connections, active health checks, passive failure detection                                     |Distribute traffic predictably and remove unhealthy backends from rotation                                             |
|11\. Resilience and traffic controls   |Per-backend concurrency limits, bounded admission queues, rate limiting, circuit breakers, restricted retries, backend draining                           |Prevent slow or failed backends from exhausting the proxy; produce controlled overload responses                       |
|12\. Operations and release hardening  |Per-backend metrics, request IDs, proxy access logs, validated configuration reload, backend membership updates, expanded fuzzing and failure tests       |Preserve active traffic during reloads, make failures observable, and pass repeatable release validation               |
|13\. API gateway routing and policies  |Hostname/path routing, per-route policies, distributed tracing, and canary traffic splitting                                                              |Apply predictable policies, correlate backend requests, and change traffic weights without interrupting active requests|
|14\. Backend response caching          |Eligible GET response storage, HTTP freshness rules, cache keys, Vary, conditional revalidation, and invalidation                                         |Serve correct reusable representations without caching private data or exceeding memory limits                         |
|15\. Cache resilience                  |Conditional stale serving, request coalescing, bounded background refresh, and failure controls                                                           |Reduce duplicate backend work and serve permitted stale responses during failures without unbounded waiters            |
|16\. WebSocket forwarding              |Validated HTTP upgrades, bidirectional forwarding, backpressure, deadlines, and connection limits                                                         |Carry long-lived WebSocket traffic over bounded buffers and close both sides safely on failure                         |
|17\. Real-time and operational hardening|Server-Sent Events, streaming policies, authenticated management API/dashboard, canary monitoring, and sustained stress tests                             |Keep traffic responsive, bound long-lived resources, and apply reviewable configuration updates safely                 |

## Detailed roadmap after Phase 12

These phases depend on the proxy, pooling, balancing, traffic controls, and
reload lifecycle from Phases 8–12. They are planned work, and their completion
requires the tests below. HTTP/2 and HTTP/3 remain separate future projects.

### Phase 13: API gateway routing and policies

**Goal:** expose several backend services through one gateway, with explicit
routing and independent policies for each route.

**Implementation:**

- Add named routes that select a backend group by hostname and path. Define
  exact-host versus wildcard-host precedence, exact-path versus prefix
  precedence, prefix boundaries, and explicit fallback behavior. Reject
  ambiguous configurations rather than depending on configuration order.
- For example, route `/users` to a users backend group and `/orders` to an
  orders backend group; hostname matching can select different services that
  share the same path.
- Keep health, readiness, metrics, and management handlers reserved. Specify
  how host ports and case are handled, and match paths separately from query
  strings. Make prefix stripping and upstream Host replacement explicit route
  settings.
- Attach authentication policy, accepted methods, body limits, connect/transfer
  deadlines, admission limits, rate limits, and quotas to each route. Reuse the
  authentication and traffic-control machinery from earlier phases.
- Define the policy order: select the route, authenticate, enforce admission
  and usage policies, then perform cache lookup or upstream forwarding. Cache
  hits must still pass the route's access and usage checks.
- Define quota identity and accounting: authenticated key or another explicitly
  configured identity, refill/reset behavior, and which requests consume quota.
  Bound the number of tracked identities and define exhaustion behavior.
  Initial counters are process-local and reset on restart; shared durable
  quotas remain later work.
- Make credential forwarding explicit. Never trust client-provided forwarding
  headers as verified identity. Document whether HMAC verification covers the
  original target before rewriting, and preserve that ordering.
- Publish validated route tables as immutable configuration snapshots. Active
  requests retain their snapshot until completion; new requests use the new
  table. Reclaim old tables only after their last owner releases them.
- Add bounded route-level metrics using configured route names, avoiding
  arbitrary URLs or user IDs as metric labels.
- Add distributed tracing: validate incoming trace context, propagate trace
  identifiers, and create a distinct upstream span context for each backend
  attempt. Correlate gateway logs, backend activity, cache hits, and errors.
  Provide an explicit trust policy for incoming context and bound or omit
  baggage so arbitrary client metadata cannot grow memory or leak sensitive
  values.
- Export gateway spans through a bounded asynchronous queue to a tracing
  collector. Configure sampling, exporter deadlines, and drop behavior;
  collector failure must not block the reactor. Record route/backend identity
  and failure stage without logging credentials or request bodies.
- Add canary destinations per route, for example 95% of eligible traffic to a
  stable backend group and 5% to a candidate group. Reuse load balancing inside
  each group, while selecting the release group separately. Validate weights
  and define behavior when the selected group is unavailable.
- Document whether allocation occurs per request or session and whether
  optional affinity applies. WebSocket/SSE sessions select a group at
  establishment and retain it; changes affect new traffic. Partition
  response-cache identity by selected release group/configuration generation so
  a cached stable response cannot unintentionally replace a canary request.
- Emit per-release request, error, timeout, and latency measurements with
  bounded configured labels. Support atomic manual promotion and rollback
  through validated configuration snapshots. Automated rollback is a later
  optional extension requiring explicit thresholds and observation windows.

**Validation and completion criteria:** test host/path precedence, unmatched
routes, rewrites, authentication boundaries, body limits, independent quotas,
counter exhaustion, and reloads while requests are active. Run simultaneous
workloads against multiple services and verify one overloaded route does not
monopolize shared resources. Test malformed and missing trace context, upstream
span correlation, sampling, full exporter queues, and collector outages. Verify
canary allocations over sufficiently large samples, zero-weight groups,
unavailable candidates, cache isolation, and weight changes during active
traffic. The phase is complete when routing and policy outcomes are
deterministic, memory use is bounded, and existing local handlers remain usable.

### Phase 14: Backend response caching

**Goal:** cache reusable backend responses while preserving their HTTP semantics
and keeping private responses out of a shared cache.

**Implementation:**

- Add a backend-response cache separate from the existing static-file content
  cache. Start with complete eligible GET responses and a documented supported
  status set. Initially bypass range requests, partial responses, streaming
  endpoints, and objects above the configured size limit.
- Build keys from route identity/configuration generation, upstream target and
  query, and the request-header values selected by Vary. Separate hostnames,
  backend services, and content-encoding variants. Treat Vary: * as uncacheable
  and bound variant count and key size.
- Default to bypassing requests with Authorization or Cookie and responses with
  Set-Cookie. Any future authenticated caching must require explicit rules for
  shared eligibility or identity isolation. Do not infer that a response is
  public simply because authentication succeeded.
- Implement a documented HTTP cache policy covering Cache-Control, shared-cache
  freshness, Date, Age, and Expires. Honor no-store and private, distinguish
  no-cache from permission to store, and keep request directives and freshness
  calculations consistent. Bypass cases the implementation cannot safely
  interpret.
- Retain validators such as ETag and Last-Modified. Revalidate stale entries
  with conditional requests, merge permitted metadata from a 304 response, and
  retain the stored body. A replacement response must complete before replacing
  the old entry.
- Stream cache misses to clients while capturing only objects within the size
  limit. Publish entries only after response framing and body completion are
  validated. Incomplete, malformed, or oversized responses never become usable
  cache entries.
- Enforce total-byte, entry-count, per-object, and variant limits. Account for
  keys, headers, metadata, capture buffers, and pinned bodies, including
  objects evicted from lookup but still referenced by clients. Reuse suitable
  cache ownership patterns from Phase 2 without weakening the total resource
  bound.
- Define invalidation by route and resource, effects of successful unsafe
  methods, and policy/backend changes. Initially provide a local administrative
  tool or reload mechanism; any network control endpoint must be authenticated
  and reserved. Never allow an old in-flight fill to republish an invalidated
  generation.
- Expose hits, misses, bypass reasons, revalidation outcomes, evictions,
  resident bytes, and in-progress capture bytes.

**Validation and completion criteria:** test freshness boundaries, Age updates,
validator changes, 304 metadata merging, Vary variants, gzip representations,
private/cookie responses, invalidation races, disconnects during fills, and
eviction with slow readers. Compare cold and warm workloads against the same
backend. The phase is complete when cache hits preserve the selected
representation and its policy, no partial response is published, and all
retained/captured data respects configured bounds.

### Phase 15: Cache resilience

**Goal:** avoid repeated backend work for the same cache entry and serve stale
content only when its policy permits it.

**Implementation:**

- Add stale-while-revalidate and stale-if-error behavior with explicit
  per-route caps. Define qualifying errors and maximum stale age. Honor
  directives that prohibit stale reuse; never override private/no-store rules
  or serve an entry invalidated by an administrative change.
- Coalesce equivalent cacheable misses or revalidations into one backend
  operation. Maintain a bounded in-flight lookup keyed by the same route,
  variant, and policy identity as the response cache. Never combine requests
  with different authentication or representation requirements.
- Start with waiters receiving the completed cache object rather than sharing a
  growing live body. This keeps follower buffering bounded. If a response
  becomes uncacheable or exceeds the object limit, release waiters through an
  explicit bounded fallback policy rather than retaining the whole response.
- Limit waiters per key, total waiters, active fills, and background refresh
  jobs. Give followers deadlines and remove them promptly on disconnect.
  Prevent cancellation of one client from leaving orphaned work or incorrectly
  canceling work still needed by other clients.
- Define failed-fill outcomes: eligible stale response, bounded independent
  forwarding, or controlled failure. Avoid releasing every waiter into a retry
  burst when a backend remains unavailable.
- Schedule background refreshes through the existing nonblocking upstream
  machinery. Limit retries and refresh frequency, and coordinate refresh
  admission with circuit breakers and backend limits.
- Keep invalidation generations and configuration snapshots attached to fills
  and refreshes. Obsolete results must not restore a revoked entry.
- Add metrics for coalesced requests, wait duration, waiter rejection, refresh
  activity, and stale responses by reason.

**Validation and completion criteria:** test simultaneous misses, backend
outages, refresh failures, slow leaders, follower cancellation, waiter
saturation, uncacheable responses, and invalidation during refresh. Verify
backend request counts as well as client outcomes. The phase is complete when
equivalent cacheable requests share bounded work, stale serving follows policy,
and failed refreshes cannot produce uncontrolled retry or memory growth.

### Phase 16: WebSocket forwarding

**Goal:** proxy long-lived WebSocket connections through the gateway using
bidirectional nonblocking I/O.

**Implementation:**

- Add an explicit WebSocket route mode with authentication, origin policy,
  upstream selection, and connection limits checked before opening a tunnel.
  Origin checks supplement authentication; they are not a substitute for it.
- Validate the HTTP upgrade request and upstream 101 response, including
  required upgrade fields, WebSocket version, accept value, and negotiated
  subprotocol/extensions. Reject an invalid handshake before switching the
  connection state. Preserve any bytes received immediately after the header
  boundary.
- Handle upgrade headers through this dedicated mode rather than the ordinary
  proxy's connection-header removal rules. Forward backend handshake rejection
  as an ordinary HTTP response when valid.
- After a successful upgrade, relay opaque WebSocket bytes in both directions.
  The client and backend remain the protocol endpoints: preserve frame bytes,
  masking, fragmentation, control frames, and negotiated extensions. Initial
  forwarding does not rewrite messages, enforce decoded message-size limits, or
  generate ping/pong frames.
- Add fixed-size buffers and independent read/write interests for both
  directions. Suspend reads when the destination buffer is full, preserve
  partial-write offsets, and bound work per reactor turn.
- Support existing client TLS and upstream TLS from Phase 9. Upgraded sockets
  are dedicated to their session and must never return to the ordinary HTTP
  connection pool.
- Enforce global, route, and backend session limits with a defined overload
  response before upgrade. Track activity-based idle deadlines and optional
  maximum session lifetime separately from ordinary HTTP response deadlines.
- Define EOF, reset, TLS shutdown, and drain behavior. Relay available
  close-frame bytes as ordinary tunnel data, use a bounded closure deadline,
  and eventually release both sockets even if the peer never completes a close
  exchange.
- Record handshake status, session lifetime, transferred bytes in each
  direction, active sessions, and termination reason without logging message
  contents.

**Validation and completion criteria:** test rejected and malformed handshakes,
subprotocol selection, text/binary traffic, fragmented frames, ping/pong and
close exchanges between endpoints, post-header bytes, partial writes, TLS, slow
readers, resets, and descriptor reuse. The phase is complete when traffic is
preserved in both directions, buffers and session counts remain bounded, and
unrelated HTTP requests remain responsive under tunnel load.

### Phase 17: Real-time and operational hardening

**Goal:** make long-lived traffic predictable under sustained load, add
Server-Sent Events (SSE) forwarding, and provide operational controls for the
gateway.

**Implementation:**

- Add an explicit SSE streaming policy for text/event-stream responses.
  Preserve event bytes and Last-Event-ID forwarding; clients and backends
  retain responsibility for event generation, reconnection, and replay. The
  gateway will not store event histories in this phase.
- Bypass response caching and whole-response buffering for SSE and upgraded
  WebSocket sessions. Forward small event batches promptly, including chunked
  responses, while preserving bounded writes and reactor fairness. Avoid
  transformations that delay delivery or alter event bytes.
- Give streaming routes separate connect, first-response, inactivity,
  slow-consumer, and optional lifetime limits. Document how heartbeat bytes
  affect inactivity and ensure continuous traffic is not terminated by an
  ordinary short response deadline.
- Enforce session budgets independently from ordinary HTTP admission, retaining
  configured headroom for health, readiness, and control requests. Bound
  metrics labels and log volume for long-lived sessions.
- Apply configuration snapshot ownership to sessions. Define which
  policy/backend changes affect only new sessions, which require draining, and
  how credential revocation affects established sessions. Do not silently
  promise immediate revocation after authentication at connection establishment.
- Support controlled backend retirement: stop admitting new sessions, allow
  existing sessions to finish within a configured grace period, then terminate
  remaining sessions. Never retry or migrate an established stream as though
  replay were safe.
- Integrate shutdown with readiness and systemd's outer timeout. Stop new
  traffic, drain existing streams for a bounded period, and document how
  clients reconnect after forced termination.
- Add streaming metrics for active sessions, directional bytes, blocked-write
  time, buffer occupancy, inactivity expirations, and drain outcomes. Measure
  event delivery latency separately from short-request latency.
- Add an authenticated management API on a separate restricted listener or
  explicitly reserved administrative routes. Separate read-only inspection from
  mutation privileges. Expose redacted configuration, route policies, backend
  health, traffic/error summaries, active streams, cache usage, and
  stable-versus-canary results; never return secret material.
- Make configuration updates reviewable: validate a candidate, return the
  proposed changes, require the expected configuration version when applying
  it, then atomically publish a new snapshot. Reject stale or invalid updates
  without disturbing active traffic. Audit administrative actions and define
  atomic persistence so applied configuration survives restart without
  weakening filesystem permissions.
- Build a management dashboard against that API. Show service/backend health,
  throughput, error rates, latency, cache behavior, active real-time sessions,
  and canary comparisons. Allow authorized operators to inspect proposed
  changes, adjust policies/weights, promote a candidate, and roll back to a
  known configuration.
- Keep dashboard refreshes and API queries bounded and separate from data-plane
  request processing. Require the appropriate management authentication and
  browser protections for the chosen session model, including CSRF protection
  for cookie-authenticated mutations. Do not expose management functionality
  through wildcard public proxy routes.

**Validation and completion criteria:** run sustained WebSocket and SSE traffic
alongside short HTTP requests, with slow consumers, bursty senders, silent
peers, backend restarts, repeated reconnects, configuration reloads, and
shutdown. Check memory and file descriptors return to expected levels after
churn. Record workload duration, event/message sizes, concurrency, latency,
errors, and resource peaks. Test management authorization, secret redaction,
concurrent/stale updates, failed persistence, audit records, dashboard
mutations, and promotion/rollback during live traffic. The phase is complete
when configured limits hold for long-lived traffic, streams deliver
progressively, draining terminates within its documented bounds, and ordinary
request responsiveness remains measurable. Administrative changes must be
validated, versioned, and observable, and monitoring failures must not stall
forwarding.

## Security and cryptography

### Client HTTPS

Extend the existing TLS support as the proxy evolves. Keep TLS handshakes and
transport I/O integrated with the nonblocking reactor.

### Upstream HTTPS

Encrypt proxy-to-backend connections and verify backend certificate chains and
hostnames. Configure trust and the expected server name explicitly.

### API tokens

Generate tokens with a cryptographically secure random source. Protect
designated routes, avoid logging credentials, and support revocation and
rotation.

### HMAC-SHA-256 request signing

Define exactly which method, request target, selected headers, and body bytes
are signed. Specify canonicalization before implementing clients. Validate
timestamps and track nonces within a bounded replay-protection window. Use
constant-time verification where appropriate.

### File integrity

Optionally expose SHA-256 digests for static assets, with trusted digest
distribution. A plain digest verifies consistency against an expected value; by
itself it does not authenticate the source.

### Key management

Keep keys and secrets outside the document root, restrict filesystem
permissions, avoid including secrets in logs or source control, and support
rotation. Use established cryptographic libraries rather than implementing
cryptographic primitives ourselves.

### Encrypted storage: conditional extension

Add AES-GCM encrypted storage only when the product introduces persisted
sensitive data. Before implementation, define the storage format, authenticated
metadata, nonce management, key lifecycle, and crash-recovery behavior.

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
- Serve static assets with caching, compression, ranges, and efficient
  large-file transfers.
- Expose operational metrics and structured access logs.
- Support readiness checks, graceful draining, and validated configuration
  reloads.

After Phases 13–17, it will additionally:

- Apply routing, authentication, and traffic policies per service.
- Correlate gateway and backend activity through distributed tracing.
- Split traffic between stable and canary releases, with observable promotion
  and rollback.
- Offer an authenticated management API and dashboard for inspection and
  validated configuration updates.
- Cache eligible backend responses with variant isolation, revalidation, and
  invalidation.
- Coalesce cache fills and serve permitted stale responses during backend
  failures.
- Forward WebSocket sessions and Server-Sent Events with bounded buffers and
  session lifetimes.

## Validation strategy

Every phase receives tests appropriate to its behavior and risk. Performance
changes are compared against the same controlled baseline.

### Performance comparisons

Record workload, connection reuse, concurrency, request counts, configuration,
logging settings, software versions, and relevant host details. Run repeated
trials and report errors alongside throughput and latency. Distinguish
functional smoke tests from capacity measurements.

### Correctness and resource handling

Exercise partial reads and writes, fragmented requests, pipelines, queue
saturation, cache ownership, disconnect cleanup, expiry, ranges, and shutdown.
Check that configured resource bounds hold under stress.

### Proxy failure scenarios

Test backend crashes, malformed responses, stalled connects and transfers,
client disconnects, unavailable backend groups, and pool exhaustion. Define
retry eligibility carefully so failures do not cause unsafe duplicate work.
Account for whether request or response bytes have already been forwarded.

### Security testing

Test incorrect and revoked tokens, altered signed requests, expired timestamps,
repeated nonces, invalid certificates, hostname mismatches, and accidental
credential exposure in logs.

### Release checks

Run strict compiler builds, integration tests, sanitizers, parser and protocol
fuzzing, stress scenarios, deployment checks, and configuration-reload tests.
Document known limitations and reproducible benchmark conditions.

## Later extensions

These are separate extensions after the first HTTP/1.1 release:

- HTTP/2 and HTTP/3.
- Distributed rate limiting.
- Multi-node coordination.
- Encrypted persistent storage when a concrete sensitive-data use case exists.

## Current progress

Phases 1–8 are implemented and locally validated. Phase 7 includes deployment
files and installation instructions; the service is not installed automatically.
Phase 8 adds optional single-backend HTTP forwarding with bounded request bodies,
streamed responses, client HTTPS, proxy metrics, and failure tests. Backend
connection reuse and verified upstream HTTPS remain planned for **Phase 9**.
Phases 13–17 are planned after Phase 12; they have not been implemented.
