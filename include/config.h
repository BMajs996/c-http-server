#ifndef CONFIG_H
#define CONFIG_H
#include <stddef.h>
typedef struct {
    unsigned port, max_connections, shutdown_ms, request_timeout_ms, response_timeout_ms;
    unsigned max_requests, file_workers, file_job_limit, log_queue_limit;
    size_t cache_bytes, cache_max_file_bytes;
    unsigned cache_entries, cache_ttl_ms;
    unsigned auth_timestamp_window_s, auth_nonce_entries;
    unsigned upstream_port, upstream_connect_timeout_ms, upstream_response_timeout_ms;
    char proxy_prefix[256], upstream_address[46], upstream_host[256];
    char bind_address[46];
    char auth_credentials_file[2048];
    char document_root[2048], access_log[2048], tls_cert[2048], tls_key[2048];
} server_config;
extern server_config config;
void config_defaults(void);
int config_load(const char *path);
#endif
