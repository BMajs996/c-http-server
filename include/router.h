#ifndef ROUTER_H
#define ROUTER_H
#include "http_parser.h"
#include "auth.h"
typedef struct {
    int status;
    const char *body,*type,*allow;
    size_t length;
} route_response;
auth_policy route_auth(const char *target);
int route_no_store(const char *target);
int route_is_api(const char *target);
/* Check routing and body policy before reading any body. Zero means accepted. */
int route_check(const struct http_request *request,const char **allow);
/* Returns zero for static fallback; buffer belongs to the connection. */
int route_dispatch(const struct http_request *request,const char *body,
                   char *buffer,size_t capacity,route_response *response);
void route_error(int status,route_response *response);
#endif
