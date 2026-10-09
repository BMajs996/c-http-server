#include "router.h"
#include "json.h"
#include "metrics.h"
#include "deployment.h"
#include <string.h>
#include <strings.h>
static int path_equal(const char *target,const char *path) {
    size_t n=strcspn(target,"?");return strlen(path)==n && !memcmp(target,path,n);
}
int route_is_api(const char *target) {
    return path_equal(target,"/api") || !strncmp(target,"/api/",5);
}
int route_no_store(const char *target) { return route_is_api(target) || path_equal(target,"/ready"); }
typedef int (*handler)(const struct http_request *,const char *,char *,size_t,route_response *);
static int status_handler(const struct http_request *r,const char *body,char *buffer,size_t capacity,route_response *out) {
    (void)r;(void)body;(void)buffer;(void)capacity;
    out->body="{\"status\":\"ok\"}\n";return 200;
}
static int ready_handler(const struct http_request *r,const char *body,char *buffer,size_t capacity,route_response *out) {
    (void)r;(void)body;(void)buffer;(void)capacity;
    int ready=deployment_ready();
    out->body=ready?"{\"status\":\"ready\"}\n":"{\"status\":\"not_ready\"}\n";
    return ready?200:503;
}
static int metrics_handler(const struct http_request *r,const char *body,char *buffer,size_t capacity,route_response *out) {
    (void)r;(void)body;
    out->length=metrics_render(buffer,capacity);out->body=out->length?buffer:NULL;
    out->type="text/plain; version=0.0.4; charset=utf-8";
    return out->length?200:500;
}
static int echo_handler(const struct http_request *r,const char *body,char *buffer,size_t capacity,route_response *out) {
    (void)buffer;(void)capacity;
    if(!json_valid(body,r->content_length))return 400;
    out->body=body;out->length=r->content_length;return 200;
}
static const struct { const char *path,*method,*allow;handler call;auth_policy policy; } routes[]={
    {"/ready","GET","GET, HEAD",ready_handler,AUTH_PUBLIC},
    {"/health","GET","GET, HEAD",status_handler,AUTH_PUBLIC},
    {"/metrics","GET","GET, HEAD",metrics_handler,AUTH_PUBLIC},
    {"/api/status","GET","GET, HEAD",status_handler,AUTH_PUBLIC},
    {"/api/echo","POST","POST",echo_handler,AUTH_PUBLIC},
    {"/api/private/status","GET","GET, HEAD",status_handler,AUTH_BEARER},
    {"/api/private/echo","POST","POST",echo_handler,AUTH_SIGNED}
};
static int find(const char *target) {
    for(size_t i=0;i<sizeof routes/sizeof routes[0];++i)if(path_equal(target,routes[i].path))return (int)i;
    return -1;
}
auth_policy route_auth(const char *target) {
    int i=find(target);return i<0?AUTH_PUBLIC:routes[i].policy;
}
void route_error(int status,route_response *out) {
    out->status=status;out->type="application/json";
    switch(status) {
    case 401:out->body="{\"error\":{\"status\":401,\"code\":\"unauthorized\"}}\n";break;
    case 503:out->body="{\"error\":{\"status\":503,\"code\":\"auth_unavailable\"}}\n";break;
    case 400:out->body="{\"error\":{\"status\":400,\"code\":\"bad_request\"}}\n";break;
    case 404:out->body="{\"error\":{\"status\":404,\"code\":\"not_found\"}}\n";break;
    case 405:out->body="{\"error\":{\"status\":405,\"code\":\"method_not_allowed\"}}\n";break;
    case 408:out->body="{\"error\":{\"status\":408,\"code\":\"request_timeout\"}}\n";break;
    case 413:out->body="{\"error\":{\"status\":413,\"code\":\"content_too_large\"}}\n";break;
    case 414:out->body="{\"error\":{\"status\":414,\"code\":\"uri_too_long\"}}\n";break;
    case 415:out->body="{\"error\":{\"status\":415,\"code\":\"unsupported_media_type\"}}\n";break;
    case 417:out->body="{\"error\":{\"status\":417,\"code\":\"expectation_failed\"}}\n";break;
    case 431:out->body="{\"error\":{\"status\":431,\"code\":\"headers_too_large\"}}\n";break;
    case 501:out->body="{\"error\":{\"status\":501,\"code\":\"transfer_encoding_unsupported\"}}\n";break;
    case 505:out->body="{\"error\":{\"status\":505,\"code\":\"http_version_unsupported\"}}\n";break;
    default:out->body="{\"error\":{\"status\":500,\"code\":\"internal_error\"}}\n";out->status=500;break;
    }
    out->length=strlen(out->body);
}
int route_check(const struct http_request *r,const char **allow) {
    int i=find(r->target);*allow=i<0?"GET, HEAD":routes[i].allow;
    if(i<0 && route_is_api(r->target))return 404;
    const char *method=i<0?"GET":routes[i].method;
    if(strcmp(r->method,method) && !(r->head && !strcmp(method,"GET")))return 405;
    if(strcmp(method,"POST"))return r->content_length?413:0;
    const char *type=r->content_type;
    size_t n=strcspn(type,";");while(n && (type[n-1]==' ' || type[n-1]=='\t'))--n;
    if(n!=16 || strncasecmp(type,"application/json",16))return 415;
    if(*r->content_encoding && strcasecmp(r->content_encoding,"identity"))return 415;
    return 0;
}
int route_dispatch(const struct http_request *r,const char *body,char *buffer,size_t capacity,route_response *out) {
    int i=find(r->target);if(i<0)return 0;
    memset(out,0,sizeof *out);out->type="application/json";out->allow=routes[i].allow;
    out->status=routes[i].call(r,body,buffer,capacity,out);
    if(out->status!=200 && !out->body)route_error(out->status,out);
    else if(!out->length)out->length=strlen(out->body);
    return 1;
}
