# C HTTP server

A Linux C11 HTTP/1.1 server with one epoll reactor, a connection state machine,
bounded asynchronous file work, metrics, graceful shutdown, and optional HTTPS.

## Build and run

On Ubuntu, install a compiler, Make, Python 3, and OpenSSL development headers:

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

To build without OpenSSL:

```sh
make clean
make TLS=0
C_HTTP_TEST_TLS=0 make TLS=0 test
```

Clean before changing TLS mode or compiler flags. A TLS=0 build rejects a
configuration requesting HTTPS. The listener binds to localhost.

## Configuration

Use `./http_server --config server.conf`; this mode cannot mix positional
arguments. Unknown keys, duplicates, invalid numbers, and an incomplete TLS
certificate/key pair fail startup. Configuration is loaded once at startup.

| Key | Default | Purpose |
| --- | --- | --- |
| port | 8080 | Listening port |
| document_root | public | Static content directory |
| max_connections | 1024 | Active connection cap |
| shutdown_ms | 5000 | Connection drain deadline |
| request_timeout_ms | 5000 | Header/idle/handshake deadline |
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

`C_HTTP_ACCESS_LOG` overrides `access_log`. Allow enough OS file descriptors
for client sockets, open files, and the reactor. Connection counts and queues
are bounded independently; raising them also raises possible memory use.

## HTTP and static files

GET and HEAD serve regular files. Directories select `index.html`; query
strings do not affect file selection. `/health` returns JSON and `/metrics`
returns Prometheus text. HTTP/1.1 keep-alive and pipelining are supported;
HTTP/1.0 persistence requires `Connection: keep-alive`.

Headers are capped at 8 KiB. Unsupported methods return 405. Ambiguous framing,
request bodies, and malformed headers are rejected and close the connection.
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
