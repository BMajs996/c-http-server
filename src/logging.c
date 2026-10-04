#define _POSIX_C_SOURCE 200809L
#include "logging.h"
#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define LOG_LIMIT config.log_queue_limit
#define RECORD_SIZE 4608
static char (*records)[RECORD_SIZE];
static size_t *sizes, head, tail, queued;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static pthread_t writer;
static int log_fd = -1, active, closing;
static atomic_uint_fast64_t dropped, written_records;
static atomic_int_fast64_t flush_deadline;
static int64_t monotonic_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) return 0;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int write_record(const char *data, size_t length) {
    while (length) {
        int64_t deadline = atomic_load(&flush_deadline);
        if (deadline && monotonic_ms() >= deadline) return -1;
        ssize_t n = write(log_fd, data, length);
        if (n > 0) { data += n; length -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = {log_fd, POLLOUT, 0};
            int rc = poll(&p, 1, 20);
            if (rc < 0 && errno != EINTR) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}
static void *log_writer(void *unused) {
    (void)unused;
    char record[RECORD_SIZE];
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!queued && !closing) pthread_cond_wait(&ready, &lock);
        if (!queued && closing) { pthread_mutex_unlock(&lock); break; }
        size_t length = sizes[head];
        memcpy(record, records[head], length);
        head = (head + 1) % LOG_LIMIT; --queued;
        pthread_mutex_unlock(&lock);
        if (write_record(record, length) < 0) atomic_fetch_add(&dropped, 1);
        else atomic_fetch_add(&written_records, 1);
    }
    return NULL;
}
int logging_init(void) {
    const char *path = getenv("C_HTTP_ACCESS_LOG");
    if (!path) path = config.access_log;
    if (path && !strcmp(path, "0")) return 0;
    if (!path || !strcmp(path, "-")) {
        /* Reopen Linux stderr as an independent nonblocking description. */
        log_fd = open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (log_fd < 0) { perror("reopen stderr for logging"); return -1; }
    } else log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK | O_CLOEXEC, 0600);
    if (log_fd < 0) { perror("access log"); return -1; }
    records=calloc(LOG_LIMIT,sizeof *records);sizes=calloc(LOG_LIMIT,sizeof *sizes);
    if(!records || !sizes){free(records);free(sizes);close(log_fd);log_fd=-1;return -1;}
    if (pthread_create(&writer, NULL, log_writer, NULL) != 0) { free(records);free(sizes);close(log_fd); log_fd = -1; return -1; }
    active = 1;
    return 0;
}
void logging_close(void) {
    if (!active) return;
    atomic_store(&flush_deadline, monotonic_ms() + 2000);
    pthread_mutex_lock(&lock); closing = 1; pthread_cond_signal(&ready); pthread_mutex_unlock(&lock);
    pthread_join(writer, NULL);
    close(log_fd); log_fd = -1; active = 0;
    free(records);free(sizes);
}
uint64_t logging_dropped(void) { return atomic_load(&dropped); }
void log_request(int fd, unsigned sequence, const char *target, const char *method,
                 int status, uintmax_t bytes, int64_t elapsed_ms, int complete) {
    if (!active) return;
    char escaped[4096]; size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)target; *p && used + 6 < sizeof escaped; ++p) {
        if (*p == '"' || *p == '\\') { escaped[used++] = '\\'; escaped[used++] = (char)*p; }
        else if (*p < 32 || *p >= 127) {
            int n = snprintf(escaped + used, sizeof escaped - used, "\\u%04x", *p);
            if (n > 0) used += (size_t)n;
        } else escaped[used++] = (char)*p;
    }
    escaped[used] = 0;
    char record[RECORD_SIZE];
    int n = snprintf(record, sizeof record,
        "{\"time_unix\":%lld,\"fd\":%d,\"request\":%u,\"method\":\"%s\","
        "\"path\":\"%s\",\"status\":%d,\"body_bytes_sent\":%ju,"
        "\"duration_ms\":%lld,\"complete\":%s,\"dropped_logs\":%ju}\n",
        (long long)time(NULL), fd, sequence, *method ? method : "-", escaped,
        status, bytes, (long long)elapsed_ms, complete ? "true" : "false",
        (uintmax_t)logging_dropped());
    if (n < 0 || (size_t)n >= sizeof record) { atomic_fetch_add(&dropped, 1); return; }
    pthread_mutex_lock(&lock);
    if (closing || queued == LOG_LIMIT) { pthread_mutex_unlock(&lock); atomic_fetch_add(&dropped, 1); return; }
    memcpy(records[tail], record, (size_t)n); sizes[tail] = (size_t)n;
    tail = (tail + 1) % LOG_LIMIT; ++queued;
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&lock);
}

uint64_t logging_written(void) { return atomic_load(&written_records); }
size_t logging_queued(void) {
    pthread_mutex_lock(&lock); size_t result = queued; pthread_mutex_unlock(&lock);
    return result;
}
