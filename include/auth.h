#ifndef AUTH_H
#define AUTH_H
#include "http_parser.h"
#include <stdint.h>
#define AUTH_ID_LIMIT 32
#define AUTH_CREDENTIAL_LIMIT 16
typedef enum { AUTH_PUBLIC, AUTH_BEARER, AUTH_SIGNED } auth_policy;
typedef struct auth_keyset auth_keyset;
struct auth_stats {
    uint64_t accepted, rejected, signatures_invalid, timestamps_invalid, replays, capacity_rejections;
    uint64_t reloads, reload_failures;
    unsigned credentials, nonce_entries;
};
/* Loading may block: startup or a file worker only. Returned set is caller-owned. */
auth_keyset *auth_load(const char *path, const char *document_root);
void auth_keyset_free(auth_keyset *set);
void auth_install(auth_keyset *set); /* Reactor publishes and takes ownership. */
int auth_init(void);
void auth_close(void);
void auth_reload_result(auth_keyset *set);
void auth_snapshot(struct auth_stats *out);
/* Zero means accepted. Early signed checks do not reserve a nonce. */
int auth_headers(const struct http_request *r, auth_policy policy);
int auth_verify(const struct http_request *r, const char *body);
/* Explicit clocks allow deterministic boundary/rollback tests. Reactor-only. */
int auth_headers_at(const struct http_request *r, auth_policy policy, int64_t wall);
int auth_verify_at(const struct http_request *r, const char *body, int64_t wall, int64_t mono_ms);
#endif
