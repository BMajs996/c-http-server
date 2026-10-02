#ifndef METRICS_H
#define METRICS_H
#include <stddef.h>
#include <stdint.h>
/* Reactor-owned counters; worker statistics are snapshotted under their locks. */
void metrics_init(unsigned connection_limit);
void metrics_accept(void);
void metrics_reject(void);
void metrics_open(void);
void metrics_close(void);
void metrics_response(int status, uintmax_t bytes, int complete, int64_t duration_ms);
void metrics_timeout(void);
void metrics_request_rejected(void);
void metrics_draining(void);
uint64_t metrics_active(void);
size_t metrics_render(char *buffer, size_t capacity);
#endif
