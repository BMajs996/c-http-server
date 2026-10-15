#ifndef UPSTREAM_POOL_H
#define UPSTREAM_POOL_H
#include "upstream.h"
#define UPSTREAM_POOL_EVENT (UINT64_C(1)<<63)
/* Reactor-owned single-backend pool; a lease belongs to exactly one request. */
int upstream_pool_init(int epoll_fd);
upstream *upstream_acquire(int64_t now,int *error);
void upstream_release(upstream *u,int reusable,int64_t now);
void upstream_pool_event(uint64_t tag,int64_t now);
void upstream_pool_tick(int64_t now);
void upstream_pool_drain(int64_t now);
int upstream_pool_pending(void);
void upstream_pool_close(void);
struct upstream_pool_stats { uint64_t total,leased,idle,closing,opened,reused,discarded,exhaustions,expired; };
void upstream_pool_snapshot(struct upstream_pool_stats *stats);
#endif
