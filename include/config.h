#ifndef CONFIG_H
#define CONFIG_H
#include <stddef.h>
#define BACKEND_LIMIT 16
enum balance_policy { ROUND_ROBIN, WEIGHTED_ROUND_ROBIN, LEAST_CONNECTIONS };
struct backend_config {
    char name[32], address[46], host[256];
    unsigned port, tls, weight, pool_limit;
    char tls_server_name[256], tls_ca_file[2048];
    char tls_client_cert[2048], tls_client_key[2048];
};
typedef struct {
    unsigned port, max_connections, shutdown_ms, request_timeout_ms, response_timeout_ms;
    unsigned max_requests, file_workers, file_job_limit, log_queue_limit;
    size_t cache_bytes, cache_max_file_bytes;
    unsigned cache_entries, cache_ttl_ms;
    unsigned auth_timestamp_window_s, auth_nonce_entries;
    unsigned upstream_connect_timeout_ms, upstream_response_timeout_ms;
    unsigned upstream_handshake_timeout_ms, upstream_read_timeout_ms, upstream_write_timeout_ms;
    unsigned upstream_pool_enabled,upstream_pool_max_connections,upstream_pool_idle_limit;
    unsigned upstream_pool_idle_timeout_ms,upstream_pool_max_age_ms,upstream_pool_max_requests;
    struct backend_config backends[BACKEND_LIMIT];
    unsigned backend_count, upstream_health_enabled;
    enum balance_policy upstream_balance;
    char upstream_group[32], upstream_health_path[2048], proxy_prefix[256];
    char bind_address[46];
    char auth_credentials_file[2048];
    char document_root[2048], access_log[2048], tls_cert[2048], tls_key[2048];
} server_config;
extern server_config config;
void config_defaults(void);
int config_load(const char *path);
#endif
