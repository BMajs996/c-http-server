#ifndef PROXY_H
#define PROXY_H
#include "http_parser.h"
#include "transport.h"
#include <stdint.h>
typedef struct proxy proxy;
enum proxy_route_kind { PROXY_LOCAL, PROXY_FORWARD, PROXY_REDIRECT };
enum proxy_route_kind proxy_route(const char *target);
int proxy_config_validate(void);
const char *proxy_allow(void);
int proxy_method(const char *method);
/* Capture/rebuild validated headers before the client's input is reused.
 * Request body is borrowed until destroy; sockets are reactor-owned. */
proxy *proxy_create(const struct http_request *request,const char *headers,size_t length,int client_fd,int *error);
void proxy_start(proxy *p,const char *body,int keep_alive,int64_t now);
/* 0 pending, 1 completed, -1 failure. Each call does bounded work.
 * client_interest and upstream_interest are independent; runnable asks the
 * reactor to schedule another bounded turn without waiting on socket events. */
int proxy_step(proxy *p,transport *client,int64_t now,uint32_t *client_interest);
/* One reactor snapshot rather than independent reads of transaction state.
 * can_send_error is false once a final write is attempted or a client write
 * must be resumed unchanged (including informational headers). */
struct proxy_progress {
    int fd, status, keep_alive, runnable, can_send_error;
    uint32_t interest;
    int64_t deadline;
    uint64_t sent;
};
struct proxy_progress proxy_progress(const proxy *p);
void proxy_drain(proxy *p);
void proxy_destroy(proxy *p);
struct proxy_stats { uint64_t active,requests,connect_failures,timeouts,invalid_responses,upstream_bytes,downstream_bytes; };
void proxy_snapshot(struct proxy_stats *stats);
#endif
