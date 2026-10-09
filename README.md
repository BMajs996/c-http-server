# C HTTP server

A Linux C11 HTTP/1.1 server with one epoll reactor, a connection state machine,
bounded asynchronous file work, metrics, graceful shutdown, and optional HTTPS.

## Build and run

On Ubuntu, install a compiler, Make, Python 3, and OpenSSL 3 development headers:

```sh
sudo apt install build-essential python3 libssl-dev openssl
make
./http_server --config server.conf
```

The example config enables a 16 MiB small-file cache. Open
http://127.0.0.1:8080, then press Ctrl+C to drain and stop.
Paths in configuration are relative to the working directory.

The original positional interface also works, with the cache disabled:

```sh
./http_server 8080 public 1024 5000
# Arguments: port, document root, connection limit, shutdown deadline in ms.
```

To build without HTTPS support (OpenSSL 3 libcrypto remains required):

```sh
make clean
make TLS=0
C_HTTP_TEST_TLS=0 make TLS=0 test
```

Clean before changing TLS mode or compiler flags. A TLS=0 build rejects a
configuration requesting HTTPS. The default listener binds to `127.0.0.1`.

## Configuration

Use `./http_server --config server.conf`; this mode cannot mix positional
arguments. Unknown keys, duplicates, invalid numbers, and an incomplete TLS
certificate/key pair fail startup. Configuration is loaded once at startup.

| Key | Default | Purpose |
| --- | --- | --- |
| bind_address | 127.0.0.1 | Numeric IPv4 or IPv6 listener address |
| port | 8080 | Listening port |
| document_root | public | Static content directory |
| max_connections | 1024 | Active connection cap |
| shutdown_ms | 5000 | Connection drain deadline |
| request_timeout_ms | 5000 | Header/body/idle/handshake deadline |
| response_timeout_ms | 30000 | Response deadline |
| max_requests | 100 | Responses allowed per connection |
| file_workers | 4 | Disk workers; 1–16 |
| file_job_limit | 64 | Outstanding disk jobs; 1–1024 |
| log_queue_limit | 1024 | Log records; 1–8192 |
| access_log | - | stderr; `0` disables; otherwise a filename |
| cache_bytes | 0 | Resident cache budget; up to 256 MiB |
| cache_max_file_bytes | 65536 | Largest cached file; up to 1 MiB |
| cache_entries | 256 | Maximum resident entries; up to 4096 |
| cache_ttl_ms | 1000 | Freshness window; 1–60000 ms |
| tls_cert, tls_key | empty | PEM certificate chain and private key |
| auth_credentials_file | empty | Private credentials outside document_root |
| auth_timestamp_window_s | 60 | Signed timestamp tolerance; 1–300 seconds |
| auth_nonce_entries | 4096 | Replay records; 1–65536 |

`C_HTTP_ACCESS_LOG` overrides `access_log`. Allow enough OS file descriptors
for client sockets, open files, and the reactor. Connection counts and queues
are bounded independently; raising them also raises possible memory use.

## Deployment and readiness

Validate inputs before starting:

```sh
./http_server --check-config server.conf
./http_server --config server.conf
# In another terminal:
curl --fail http://127.0.0.1:8080/ready
```

`--check-config` checks configuration, numeric bind address, document root, TLS,
credentials, and log access without binding, starting threads, sending
notifications, or creating log files. Port availability and runtime resources
are checked when the server starts.

Set `bind_address` to a numeric IPv4 or IPv6 address. IPv6 listeners are
IPv6-only, and one process uses one listener. Hostnames, brackets, and scope
suffixes are rejected. Wildcard addresses `0.0.0.0` and `::` expose public routes
on all interfaces of that address family.

`GET /ready` returns `200` with `{"status":"ready"}` after initialization.
The unready response is `503` with `{"status":"not_ready"}`. HEAD returns the
same headers without a body; responses use `Cache-Control: no-store`.
`c_http_ready` reports the same lifecycle state. Draining clears readiness and
closes the listener immediately, so new probes usually fail to connect.
Readiness does not test filesystem responsiveness or authentication availability.

For installation under a dedicated account, use the
[systemd deployment guide](deploy/README.md). The example service uses
`Type=notify`, journal logging, resource limits, and filesystem restrictions.
Manual runs need no notification socket. If `NOTIFY_SOCKET` is supplied,
startup requires successful nonblocking delivery of `READY=1`; shutdown sends
`STOPPING=1` on a best effort basis. Only authentication credentials reload on
SIGHUP; changes to other settings require restarting.

## Project layout

- `src/`: application C sources.
- `include/`: application headers.
- `tests/`: integration tests and C cache, authentication, and deployment tests.
- `deploy/`: systemd service, production configuration example, and installation guide.
- `fuzz/`: parser, range, and JSON fuzzing.
- `tools/`: benchmarks, stress tests, and asset preparation.
- `public/`: static assets.

## HTTP and static files

GET and HEAD serve regular files. Directories select `index.html`; query
strings do not affect file selection. `/health` returns JSON and `/metrics`
returns Prometheus text. HTTP/1.1 keep-alive and pipelining are supported;
HTTP/1.0 persistence requires `Connection: keep-alive`.

Headers are capped at 8 KiB. Unsupported route methods return 405 with `Allow`.
Ambiguous framing and malformed headers are rejected and close the connection.
Static files, health, readiness, and metrics reject nonempty request bodies.
Uncached plain HTTP files use `sendfile()` to nonblocking client sockets in
bounded 64 KiB calls.
If the kernel cannot use `sendfile()`, the response resumes through the worker
pool from the exact unsent offset. HTTPS files use worker-backed 32 KiB chunks.
Large files do not require whole-file buffers. The worker pool performs file
opening and fallback reads outside the reactor.
The kernel can still stall a `sendfile()` call while fetching file pages from
slow storage; the socket readiness guarantee does not make disk I/O asynchronous.

Static responses provide MIME types, `nosniff`, weak metadata ETags,
Last-Modified, and `Cache-Control: no-cache`. If-None-Match supports 304.
Single byte ranges support 206 and unsatisfiable ranges return 416. HEAD ignores
Range. If-Range conservatively serves the full representation. Multipart ranges
are not implemented.

URL paths are decoded once. Dot-prefixed components, control characters,
backslashes, and ambiguous nested escapes are rejected. Traversal uses `openat`
and `O_NOFOLLOW`; symlinks and non-regular files are not served. Keep only
publishable content under the document root. These checks do not protect secrets
copied or hard-linked into that directory. Publish updates with atomic rename;
in-place modification can change an ongoing uncached transfer.

## File content cache

Set `cache_bytes` above zero to enable it. Eligible files are preloaded by disk
workers and retained as immutable content. Warm hits avoid filesystem work.
Cached GET, HEAD, ranges, and conditional responses share the same metadata.

Entries expire after `cache_ttl_ms`: changes can remain invisible during this
window. There is no instant file watcher or cache purge endpoint. Expired entries
are replaced on the next request. Least recently used entries are evicted;
active responses pin their data until completion. Pinned and retired entries
continue to count against the byte and entry limits. If capacity is unavailable,
the response falls back to streaming.

The byte budget includes entry metadata, each path's actual length, and content.
Cached entries retain the response metadata they need, without unused file
descriptor and filesystem `stat` fields. The path hash table uses
additional memory, bounded by 8,192 pointers (64 KiB on a 64-bit host).
Worker staging buffers are additional memory, bounded by
`file_job_limit * cache_max_file_bytes`, plus allocator overhead. TLS also has
OpenSSL-managed buffers. The cache budget is not a cap on total process memory.

## HTTPS

Provide both `tls_cert` and `tls_key` to make the configured listener HTTPS.
The nonblocking handshake, reads, writes, and close notification run through
epoll. TLS 1.2 is the minimum; ALPN supports HTTP/1.1. HTTP/2 is not implemented.
The server has one listener, so HTTP and HTTPS are alternative modes.

For local development, create a temporary certificate outside the public root:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -days 7 \
  -keyout dev-key.pem -out dev-cert.pem -subj '/CN=localhost' \
  -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1'
```

Set these in a copy of `server.conf`:

```ini
tls_cert = dev-cert.pem
tls_key = dev-key.pem
```

Then start with that config and verify the certificate:

```sh
curl --cacert dev-cert.pem https://localhost:8080/health
python3 tools/benchmark.py --https --ca-file dev-cert.pem --requests 1000
```

Use an appropriate trusted certificate for deployment. Protect the private key
and keep it outside the document root. Certificates load on startup; restart to
reload them. No certificates or private keys are included in this project.

## Logging, metrics, and shutdown

Access logs are JSON lines written by a background thread. A full log queue
drops records rather than blocking request handling. Metrics include connections,
status classes, completed/aborted responses, bytes, timeouts, duration histogram,
worker queues, logging drops, cache activity, and TLS handshakes.

```sh
curl http://127.0.0.1:8080/metrics
C_HTTP_ACCESS_LOG=results/access.jsonl ./http_server --config server.conf
```

Metrics are captured before their own response completes, so the current scrape
is not yet included in completed-response counters. Durations measure server
response lifecycle time, including disk work and output backpressure.

SIGINT/SIGTERM stop accepting connections, close idle and incomplete requests,
and finish the current response on active connections. The deadline or a second
signal closes remaining sockets. Worker cleanup waits for active filesystem
operations; a stuck filesystem can delay process exit beyond the socket deadline.
Logging gets a bounded final flush.

## Routing and JSON APIs

A method/path table in `src/router.c` dispatches built-in handlers before static-file
fallback. Route paths are case sensitive and matched literally without query
strings; percent-encoded aliases and trailing slashes do not match API routes.
The `/api` namespace is reserved: unknown paths return JSON 404 errors.

| Method | Path | Response |
| --- | --- | --- |
| GET, HEAD | `/health` | Existing JSON health response |
| GET, HEAD | `/ready` | Initialization and draining readiness; no-store |
| GET, HEAD | `/metrics` | Existing Prometheus metrics |
| GET, HEAD | `/api/status` | `{"status":"ok"}` |
| POST | `/api/echo` | The validated JSON request body, unchanged |

```sh
curl http://127.0.0.1:8080/api/status
curl -i http://127.0.0.1:8080/api/echo \
  -H 'Content-Type: application/json' --data '{"message":"hello","count":3}'
```

Echo accepts any complete UTF-8 JSON value, including arrays and scalars.
Validation checks strings, escapes, numbers, literals, and at most 32 nested
containers. It preserves whitespace and duplicate object keys and does not
convert numeric values or normalize Unicode escapes. `Content-Type` must be
`application/json` (parameters are ignored); content encoding must be absent or
`identity`. Other media types or compressed request bodies return 415.

Bodies have a fixed 16 KiB limit (`BODY_LIMIT` in `include/http_parser.h`). Only echo
accepts nonempty bodies. The server allocates exactly the declared body length
for accepted requests and releases it after the response or connection teardown.
Without Content-Length, the body is empty and echo returns 400. Reads are
nonblocking and preserve subsequent pipelined requests. The request deadline
covers both headers and body and does not reset as bytes arrive. Incomplete
bodies return 408 on timeout and are closed during shutdown.

Transfer-Encoding is unsupported (501), Expect is unsupported (417), and
conflicting framing is rejected (400). Oversized bodies return 413 before body
allocation. Header, framing, routing, and media-type rejections close the
connection. Invalid JSON returns 400 after consuming the complete body and can
retain keep-alive. API responses use `Cache-Control: no-store`; errors use
`{"error":{"status":400,"code":"bad_request"}}` with the appropriate status and
stable code. Errors before an API target can be parsed use the ordinary HTTP
error response. JSON response bodies are not gzip-compressed.

To add a handler, register its path, method, Allow value, and function in the
route table. The function receives the parsed request, bounded body, connection
response buffer, and response descriptor. Returned body memory must remain valid
until the response finishes; use literals, the supplied buffer, or the request
body. Add body/media policy to `route_check` when introducing another body format.

## Authentication and signed requests

Phase 6 adds two protected demonstration routes:

| Method | Route | Required authentication |
| --- | --- | --- |
| GET, HEAD | `/api/private/status` | Bearer token |
| POST | `/api/private/echo` | HMAC-SHA-256 signed request |

Existing health, metrics, status, echo, and static routes keep their public
policies. Protected routes return 401 when credentials are not configured; they
never become public. A bearer token cannot replace a required signature.

### Generate credentials and run locally

Create a private directory outside `public/`. The following files are ignored
by Git. Each generator invocation creates new files and refuses overwrites:

```sh
mkdir -p .secrets
chmod 700 .secrets
python3 tools/create_credentials.py --kind token --id token-a \
  --client-file .secrets/token.auth-client --server-file .secrets/token.credentials
python3 tools/create_credentials.py --kind hmac --id sign-a \
  --client-file .secrets/sign.auth-client --server-file .secrets/sign.credentials
(umask 077; cat .secrets/token.credentials .secrets/sign.credentials > .secrets/server.credentials)
make
./http_server --config server-auth.conf
```

Stop any previous server on port 8080 first. In another terminal:

```sh
# No credentials: expect 401.
curl -i http://127.0.0.1:8080/api/private/status
# Token authentication: expect 200 and status JSON.
python3 tools/auth_client.py --credential .secrets/token.auth-client
# Signed JSON echo: expect 200 and {}.
python3 tools/auth_client.py --credential .secrets/sign.auth-client
# Sign an exact JSON file:
python3 tools/auth_client.py --credential .secrets/sign.auth-client --body-file payload.json
```

The generator uses 32 random bytes for each independent secret and prints no
secrets. The client file contains the secret. The server file contains the token
secret's SHA-256 digest, or the actual HMAC key needed for verification. Records
use `token ID HEX_DIGEST` or `hmac ID HEX_KEY`. IDs contain 1–32 ASCII letters,
digits, underscores, or hyphens, and must be unique across both types. Digests and
keys use exactly 64 lowercase hexadecimal characters. Up to 16 records are
accepted; comments start with `#`, and an empty file revokes all credentials.

The server requires a regular credential file owned by its effective user,
with no group/other permissions, one hard link, and no final symlink. It must be
outside the resolved document root and at most 8 KiB. Use private parent
directories and publish updates by atomic file replacement. Invalid configured
credentials prevent startup.

### Configuration and rotation

The example `server-auth.conf` points to `.secrets/server.credentials`:

```ini
auth_credentials_file = .secrets/server.credentials
auth_timestamp_window_s = 60
auth_nonce_entries = 4096
```

The timestamp window supports 1–300 seconds and the replay store 1–65,536 entries.
These are startup settings. SIGHUP reloads credentials only, using the bounded
file-worker pool. It preserves the current credentials if loading fails and
preserves replay records on success. Requests read credential files only at
startup or reload, never on the authentication path.

To rotate, generate a new ID, atomically publish a file containing old and new
records, then send SIGHUP to the server PID. Update clients, publish a file with
the old record removed, and send SIGHUP again. Check stderr or the authentication
reload counters to confirm success. Removing an ID rejects subsequent checks,
including requests on existing keep-alive connections and signed requests still
reading their bodies. Already authenticated requests may finish.

### Signing contract

Signed requests send these headers, once each:

```text
X-Auth-Key-Id: <credential ID>
X-Auth-Timestamp: <Unix seconds, decimal, no leading zeroes>
X-Auth-Nonce: <16 random bytes as 32 lowercase hexadecimal characters>
X-Auth-Signature: <HMAC as 64 lowercase hexadecimal characters>
```

The MAC input is the following ASCII fields, each followed by a newline,
including the last field:

```text
C-HTTP-HMAC-V1
key-id
timestamp
nonce
method
original-request-target
content-type
content-encoding
content-length
body-sha256
```

The method and target are exact parsed bytes. The target includes the query
string, original percent encoding, and original parameter order. Selected header
values have only surrounding spaces/tabs removed; a missing header contributes
an empty field. Content-Length is the actual bounded body length as normalized
decimal, and the body digest is lowercase hexadecimal SHA-256 of exact bytes.
JSON whitespace is significant. Host is not signed; provision separate signing
keys for different services. Signing happens before the server strips queries.
The server uses OpenSSL SHA-256, HMAC, and constant-time secret comparisons.

Header and routing checks precede body allocation. Signature verification and
nonce reservation happen after the complete body arrives and before the handler
runs. Failed signatures do not consume nonce capacity. Successfully authenticated
requests consume their nonce even when the handler rejects the JSON body.

### Replay bounds, errors, and transport

Replay records are scoped by credential ID and nonce, with keyed hash lookup.
They survive at least `2 * timestamp_window + 1` seconds on the monotonic clock
and until the original timestamp can no longer pass wall-clock validation.
Cleanup retires at most 32 oldest entries per signed request. Conservative
retention can keep records longer during clock changes. Backward clock movement
observed during signing checks returns 503 until time catches up.

An unexpired full store returns 503 and retains its replay records. Size it for
all distinct signed requests during the retention period. With the default
60-second window and 4,096 entries, a sustained workload can fill it at roughly
34 signed requests/s. Larger workloads need a higher entry limit or a smaller
window. At the maximum entry count, the replay structures use about 4 MiB.
Replay state is per process, resets on restart, and is not shared across servers.

Authentication failures return generic JSON 401 errors; malformed fields return
400, oversized fields 431, and unsafe clock/capacity conditions 503. Early
rejections and signature failures close the connection. Bearer failures send
`WWW-Authenticate: Bearer`. Credentials, signatures, and auth headers are not
logged. Metrics expose totals for accepted/rejected authentication, invalid
signatures/timestamps, replays, capacity failures, and credential reloads.

Use HTTPS for real credentials over a network: bearer tokens grant access to
whoever possesses them, and HMAC authenticates without encrypting request data.
The client accepts `--https --host localhost --ca-file dev-cert.pem` and verifies
certificates. Plain HTTP in the commands above is for local loopback testing.

### Authenticated benchmarks

The benchmark tool now supports POST bodies and private client credentials:

```sh
python3 tools/benchmark.py --path /api/private/status \
  --credential .secrets/token.auth-client --requests 1000 --trials 3
python3 tools/benchmark.py --path /api/private/echo --method POST \
  --credential .secrets/sign.auth-client --requests 500 --trials 3
```

POST defaults to `{}`; use `--body-file payload.json` for another payload.
Every signed request, including warmup, gets a fresh nonce and signature. Results
record method, authentication type, body length, and body digest without secrets.
Saved comparisons require matching method, authentication, and payload. Compare
public/authenticated summaries separately when assessing authentication overhead.
Client signing is included in end-to-end latency. Repeated signed benchmark runs
share the server's replay capacity until records expire; increase its limit for
larger runs rather than treating expected 503 capacity responses as crypto faults.

## Precompressed gzip assets

Generate sidecars before starting the server or publishing assets:

```sh
make precompress
# Or compress another document root:
python3 tools/precompress.py /path/to/assets
```

The tool supports HTML, CSS, JavaScript, JSON, text, and SVG. It creates
deterministic `.gz` files, verifies their decompressed content, and atomically
replaces each sidecar. Outputs that are not smaller are omitted, and existing
sidecars for those files are removed. Hidden files and symlinks are skipped.

Static requests negotiate gzip and identity using `Accept-Encoding` quality
values. Gzip wins ties when a usable sidecar exists. Missing or empty headers
select identity; repeated codings use their lowest quality. If neither available
representation is acceptable, the server returns 406. Malformed quality values
return 400.

Both representations send `Vary: Accept-Encoding` and separate ETags. Gzip
responses preserve the original MIME type and send `Content-Encoding: gzip` and
the compressed length. HEAD and conditional requests use the selected
representation. Gzip ignores Range and returns the full response with
`Accept-Ranges: none`; identity retains byte-range support. Cache variants share
the configured byte and entry limits. HTTP uses the existing sendfile path for
uncached bodies; HTTPS uses worker reads.

The original file must exist. Sidecars must be regular files without symlinks,
have gzip magic bytes, and have a modification time at least as recent as the
original. The server does not verify the full gzip stream or its correspondence
to the original; publish verified originals and sidecars together, preferably
while the server is stopped. Cached responses can remain until cache TTL expiry.
A gzip preference with an unavailable sidecar requires a worker lookup on each
request before falling back to identity.

After generating sidecars and starting the server:

```sh
curl -I -H 'Accept-Encoding: gzip' http://127.0.0.1:8080/assets/style.css
curl --compressed http://127.0.0.1:8080/assets/style.css
curl -I -H 'Accept-Encoding: identity' http://127.0.0.1:8080/assets/style.css
```

For a gzip benchmark, add `--accept-encoding gzip` to `tools/benchmark.py`.
The result records this header and counts transferred compressed body bytes.
Saved-result comparisons require matching encoding headers; examine separate
identity and gzip summaries when comparing transfer sizes.

## Tests and fuzzing

```sh
make test
make fuzz
# Optional: Clang with libFuzzer
make fuzz_libfuzzer
./fuzz_libfuzzer fuzz/corpus -runs=100000 -max_len=8224
```

Tests cover parsing, framing, paths, ranges, caching, verified HTTPS, pipelining,
queue saturation, delayed disk work, slow output, metrics, logging, and shutdown.
A C cache unit test checks ownership, pinned entries, and capacity. HTTPS tests
create temporary certificates and require the `openssl` command. An optional
file-descriptor check skips when the process descriptor table is unavailable.

For sanitizers, clean and pass the same flags to both build and test:

```sh
make clean
make test CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -g -O1 -fsanitize=address,undefined'
```

`.github/workflows/ci.yml` runs GCC/Clang strict builds, HTTP-only builds,
integration tests, sanitizers, deterministic fuzzing, and coverage-guided fuzzing.
Push the contents of this directory as the repository root to activate it.
The workflow is supplied but has not been executed on GitHub in this session.

## Benchmarks and stress

Start the server in another terminal, then:

```sh
mkdir -p results
python3 tools/benchmark.py --path /health --requests 100000 --concurrency 32 \
  --output results/health-32.json
python3 tools/benchmark.py --path /assets/style.css --requests 100000 \
  --concurrency 32 --output results/static-32.json
python3 tools/benchmark.py --requests 10000 --concurrency 32 --fresh --warmup 0 \
  --output results/fresh-baseline.json
python3 tools/stress.py --idle 300 --slow 8 --slow-path /large.bin \
  --requests 10000 --concurrency 32 > results/stress.json
```

Create a large test file under the document root for sustained slow-reader
backpressure. Idle sockets can expire during a long stress run. HTTPS is available
in both tools with `--https --ca-file CERT.pem`; certificate validation stays on.

Compare identical paths, concurrency, connection reuse, logging, configuration,
and request counts. Run several trials. The bundled Python client and loopback
host can limit measured throughput; these results do not isolate server capacity
or establish a language comparison. Existing results are snapshots from earlier
versions, not measurements of the new cache or HTTPS implementation.

The benchmark runs five trials by default and saves individual results plus
median throughput and p50/p95/p99 latency for trials without errors. Record the
server configuration with `--config`; the result stores its path and SHA-256
hash, alongside the workload, Python version, platform, CPU count, and UTC time.
Use `--label` to identify a run. For a cache comparison, start the server with
`cache_bytes = 0`, run the benchmark, then restart with the same configuration
except for a nonzero `cache_bytes` value and run it again. Keep the listening
port, logging setting, workload, and host the same:

```sh
python3 tools/benchmark.py --path /assets/style.css --requests 10000 \
  --concurrency 32 --trials 5 --config server.conf --label cache-off \
  --output results/cache-off.json
# Restart the server with cache enabled before the next command.
python3 tools/benchmark.py --path /assets/style.css --requests 10000 \
  --concurrency 32 --trials 5 --config server.conf --label cache-on \
  --output results/cache-on.json
python3 tools/benchmark.py --compare results/cache-off.json results/cache-on.json
```

The comparison rejects different workloads and reports errors as well as median
changes. It accepts older single-trial result files. A result with any failed
trial exits unsuccessfully; comparison also exits unsuccessfully when errors
are present. Run each variant more than once and alternate their order if small
differences matter, since shared-host load can change between runs.

For many-file cache measurements, create a text file with one URL path per line
and pass `--paths-file paths.txt`. The tool cycles through these paths for warmup
and measured requests. It records the ordered path list's SHA-256 digest and
rejects comparisons with different path lists. Use a TTL long enough that all
files remain fresh during a trial, and a cache entry limit large enough for the
working set when measuring warm cache lookups.

`make benchmark-cache` measures 300,000 direct warm lookups across 3,000 cache
entries. It isolates the cache data structure from HTTP, sockets, and the Python
client. Run it on an otherwise idle host for a useful local comparison.
