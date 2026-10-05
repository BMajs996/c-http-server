#define _POSIX_C_SOURCE 200809L
#include "metrics.h"
#include "config.h"
#include "file_cache.h"
#include "transport.h"
#include "file_io.h"
#include "logging.h"
#include "auth.h"
#include <stdio.h>
#include <time.h>
static uint64_t accepted, rejected, active, completed, aborted, bytes_sent, timeouts, invalid;
static uint64_t status_counts[6], latency_counts[8], duration_ms_sum;
static unsigned limit, draining;
static int64_t started;
static int64_t now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) return 0;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
void metrics_init(unsigned maximum) { limit = maximum; started = now_ms(); }
void metrics_accept(void) { ++accepted; }
void metrics_reject(void) { ++rejected; }
void metrics_open(void) { ++active; }
void metrics_close(void) { if (active) --active; }
void metrics_timeout(void) { ++timeouts; }
void metrics_request_rejected(void) { ++invalid; }
void metrics_draining(void) { draining = 1; }
uint64_t metrics_active(void) { return active; }
void metrics_response(int status, uintmax_t bytes, int complete, int64_t elapsed) {
    bytes_sent += bytes;
    if (!complete) { ++aborted; return; }
    ++completed;
    if (status >= 100 && status < 600) ++status_counts[status / 100];
    if (elapsed < 0) elapsed = 0;
    duration_ms_sum += (uint64_t)elapsed;
    const uint64_t buckets[] = {1, 5, 10, 25, 50, 100, 1000};
    for (size_t i = 0; i < 7; ++i) if ((uint64_t)elapsed <= buckets[i]) ++latency_counts[i];
    ++latency_counts[7];
}
size_t metrics_render(char *buffer, size_t capacity) {
    struct file_io_stats io;
    file_io_snapshot(&io);
    struct file_cache_stats cache;file_cache_snapshot(&cache);
    struct auth_stats auth;auth_snapshot(&auth);
    size_t used = 0;
#define PUT(...) do { int n = snprintf(buffer + used, capacity - used, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= capacity - used) { return 0; } used += (size_t)n; } while (0)
#define COUNTER(name, value) PUT("# TYPE c_http_" name " counter\nc_http_" name " %ju\n", (uintmax_t)(value))
#define GAUGE(name, value) PUT("# TYPE c_http_" name " gauge\nc_http_" name " %ju\n", (uintmax_t)(value))
    GAUGE("uptime_seconds", (now_ms() - started) / 1000);
    GAUGE("active_connections", active);
    GAUGE("connection_limit", limit);
    GAUGE("draining", draining);
    COUNTER("connections_accepted_total", accepted);
    COUNTER("connections_rejected_total", rejected);
    COUNTER("responses_completed_total", completed);
    COUNTER("responses_aborted_total", aborted);
    COUNTER("body_bytes_sent_total", bytes_sent);
    COUNTER("timeouts_total", timeouts);
    COUNTER("request_rejections_total", invalid);
    PUT("# TYPE c_http_responses_total counter\n");
    for (unsigned i = 1; i < 6; ++i)
        PUT("c_http_responses_total{status_class=\"%uxx\"} %ju\n", i, (uintmax_t)status_counts[i]);
    GAUGE("file_jobs_outstanding", io.outstanding);
    GAUGE("file_jobs_queued", io.queued);
    GAUGE("file_jobs_running", io.running);
    GAUGE("file_jobs_completed_pending", io.completed_pending);
    GAUGE("file_job_limit", io.limit);
    COUNTER("file_jobs_submitted_total", io.submitted);
    COUNTER("file_jobs_finished_total", io.finished);
    GAUGE("log_queue_depth", logging_queued());
    GAUGE("log_queue_limit", config.log_queue_limit);
    COUNTER("logs_written_total", logging_written());
    COUNTER("logs_dropped_total", logging_dropped());
    GAUGE("tls_enabled",transport_tls_enabled());
    COUNTER("tls_handshakes_total",transport_handshakes());
    COUNTER("tls_failed_handshakes_total",transport_failed_handshakes());
    GAUGE("cache_bytes", cache.bytes);
    GAUGE("cache_byte_limit", config.cache_bytes);
    GAUGE("cache_entries", cache.entries);
    COUNTER("cache_hits_total", cache.hits);
    COUNTER("cache_misses_total", cache.misses);
    COUNTER("cache_evictions_total", cache.evictions);
    COUNTER("cache_expirations_total", cache.expirations);
    COUNTER("cache_bypasses_total", cache.bypasses);
    GAUGE("auth_credentials",auth.credentials);
    GAUGE("auth_nonce_entries",auth.nonce_entries);
    GAUGE("auth_nonce_limit",config.auth_nonce_entries);
    COUNTER("auth_accepted_total",auth.accepted);
    COUNTER("auth_rejected_total",auth.rejected);
    COUNTER("auth_invalid_signatures_total",auth.signatures_invalid);
    COUNTER("auth_invalid_timestamps_total",auth.timestamps_invalid);
    COUNTER("auth_replays_total",auth.replays);
    COUNTER("auth_capacity_rejections_total",auth.capacity_rejections);
    COUNTER("auth_reloads_total",auth.reloads);
    COUNTER("auth_reload_failures_total",auth.reload_failures);
    PUT("# TYPE c_http_response_duration_seconds histogram\n");
    const char *bounds[] = {"0.001", "0.005", "0.010", "0.025", "0.050", "0.100", "1.000", "+Inf"};
    for (unsigned i = 0; i < 8; ++i)
        PUT("c_http_response_duration_seconds_bucket{le=\"%s\"} %ju\n", bounds[i], (uintmax_t)latency_counts[i]);
    PUT("c_http_response_duration_seconds_sum %.3f\nc_http_response_duration_seconds_count %ju\n",
        (double)duration_ms_sum / 1000.0, (uintmax_t)completed);
#undef PUT
#undef COUNTER
#undef GAUGE
    return used;
}
