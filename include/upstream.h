#ifndef UPSTREAM_H
#define UPSTREAM_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef struct upstream upstream;
int upstream_init(void);
void upstream_close(void);
/* One reactor owns a connection. I/O: bytes, 0 EOF, -1 error, -2 retry. */
upstream *upstream_open(int64_t now,int *error);
/* Ready/shutdown: 1 complete, 0 pending, -1 failed; clean: 1 safe, 0 discard. */
int upstream_ready(upstream *u,int64_t now,uint32_t *interest);
ssize_t upstream_read(upstream *u,void *data,size_t size,uint32_t *interest);
ssize_t upstream_write(upstream *u,const void *data,size_t size,uint32_t *interest);
int upstream_clean(upstream *u,uint32_t *interest);
int upstream_shutdown(upstream *u,uint32_t *interest);
void upstream_begin(upstream *u,int64_t now);
enum upstream_operation { UPSTREAM_NONE, UPSTREAM_READ, UPSTREAM_WRITE };
struct upstream_state {
    int fd;enum upstream_operation pending;
    int64_t connected_at,born;unsigned requests;
};
struct upstream_tls_stats { uint64_t handshakes,handshake_failures,verification_failures,io_failures; };
void upstream_tls_snapshot(struct upstream_tls_stats *stats);
struct upstream_state upstream_state(const upstream *u);
void upstream_destroy(upstream *u);
#endif
