#ifndef UPSTREAM_HEALTH_H
#define UPSTREAM_HEALTH_H
#include <stdint.h>
#define UPSTREAM_HEALTH_EVENT (UINT64_C(1)<<62)
struct upstream_health_stats {
    uint64_t probe_ok, probe_failed, passive_failed;
    int healthy;
};
int upstream_healthy(unsigned index,int64_t now);
void upstream_health_result(unsigned index,int success,int64_t now);
void upstream_health_snapshot(unsigned index,int64_t now,struct upstream_health_stats *out);
void upstream_health_tick(int epoll_fd,int64_t now);
void upstream_health_event(int epoll_fd,uint64_t tag,int64_t now);
void upstream_health_stop(void);
#endif
