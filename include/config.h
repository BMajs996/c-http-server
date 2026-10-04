#ifndef CONFIG_H
#define CONFIG_H
#include <stddef.h>
typedef struct {
    unsigned port, max_connections, shutdown_ms, request_timeout_ms, response_timeout_ms;
    unsigned max_requests, file_workers, file_job_limit, log_queue_limit;
    size_t cache_bytes, cache_max_file_bytes;
    unsigned cache_entries, cache_ttl_ms;
    char document_root[2048], access_log[2048], tls_cert[2048], tls_key[2048];
} server_config;
extern server_config config;
void config_defaults(void);
int config_load(const char *path);
#endif
