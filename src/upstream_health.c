#include "upstream_health.h"
#include "upstream_pool.h"
#include "config.h"
#include "http_response.h"
#include "http_syntax.h"
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>

enum { PROBE_LIMIT=4, INTERVAL_MS=5000, PROBE_TIMEOUT_MS=2000, COOLDOWN_MS=10000, CLOSE_MS=1000 };
struct health {
    unsigned active_failures, passive_failures, recoveries;
    int64_t down_until, next_probe;
    struct upstream_health_stats counters;
};
struct probe {
    upstream *socket;
    unsigned backend, informational;
    int ready, closing;
    uint64_t tag;
    int64_t deadline;
    size_t sent, length, used, scan, info_bytes;
    char request[sizeof config.upstream_health_path+sizeof config.backends[0].host+64];
    char headers[HEADER_LIMIT];
};
static struct health members[BACKEND_LIMIT];
static struct probe probes[PROBE_LIMIT];
static unsigned next_member;
static uint64_t sequence=1;

int upstream_healthy(unsigned index,int64_t now) {
    return !members[index].down_until || now>=members[index].down_until;
}
static void quarantine(unsigned index,int64_t now) {
    struct health *h=&members[index];
    if(!upstream_healthy(index,now))return;
    h->down_until=config.upstream_health_enabled?INT64_MAX:now+COOLDOWN_MS;
    h->recoveries=0;
    h->passive_failures=0;
    upstream_pool_drop_idle(index,now);
}
void upstream_health_result(unsigned index,int success,int64_t now) {
    if(!*config.upstream_group && !config.upstream_health_enabled)return;
    struct health *h=&members[index];
    if(success)h->passive_failures=0;
    else {
        h->counters.passive_failed++;
        if(upstream_healthy(index,now) && ++h->passive_failures>=2)quarantine(index,now);
    }
}
static void probe_result(unsigned index,int success,int64_t now) {
    struct health *h=&members[index];
    if(success) {
        h->counters.probe_ok++;
        h->active_failures=0;
        if(!upstream_healthy(index,now) && ++h->recoveries>=2) {
            h->down_until=0;
            h->recoveries=0;
        }
    } else {
        h->counters.probe_failed++;
        h->recoveries=0;
        if(h->active_failures<2)h->active_failures++;
        if(h->active_failures>=2)quarantine(index,now);
    }
}
void upstream_health_snapshot(unsigned index,int64_t now,struct upstream_health_stats *out) {
    *out=members[index].counters;
    out->healthy=upstream_healthy(index,now);
}
static void drop(struct probe *p) {
    upstream_destroy(p->socket);
    p->socket=NULL;
}
static void watch(int epoll_fd,struct probe *p,uint32_t wanted,int operation) {
    struct epoll_event event={.events=wanted|EPOLLRDHUP,.data.u64=p->tag};
    if(epoll_ctl(epoll_fd,operation,upstream_state(p->socket).fd,&event)<0)drop(p);
}
static int probe_step(struct probe *p,int64_t now,uint32_t *wanted) {
    if(!p->ready) {
        int result=upstream_ready(p->socket,now,wanted);
        if(result<=0)return result;
        p->ready=1;
    }
    while(p->sent<p->length) {
        ssize_t n=upstream_write(p->socket,p->request+p->sent,p->length-p->sent,wanted);
        if(n<=0)return n==-2?0:-1;
        p->sent+=(size_t)n;
    }
    for(;;) {
        size_t length=http_header_end(p->headers,p->used,&p->scan);
        if(length) {
            struct http_response response;
            if(http_response_parse(&response,p->headers,length,0)<0)return -1;
            if(response.status>=200)return response.status<300?1:-1;
            if(++p->informational>8 || (p->info_bytes+=length)>HEADER_LIMIT)return -1;
            p->used-=length;p->scan=0;
            memmove(p->headers,p->headers+length,p->used);
            continue;
        }
        if(p->used==sizeof p->headers)return -1;
        ssize_t n=upstream_read(p->socket,p->headers+p->used,sizeof p->headers-p->used,wanted);
        if(n<=0)return n==-2?0:-1;
        p->used+=(size_t)n;
    }
}
static void advance(int epoll_fd,struct probe *p,int64_t now,int operation) {
    uint32_t wanted=0;
    if(p->closing) {
        if(now>=p->deadline || upstream_shutdown(p->socket,&wanted)!=0)drop(p);
        else watch(epoll_fd,p,wanted,operation);
        return;
    }
    int result=now>=p->deadline?-1:probe_step(p,now,&wanted);
    if(!result){watch(epoll_fd,p,wanted,operation);return;}
    probe_result(p->backend,result>0,now);
    p->closing=1;p->deadline=now+CLOSE_MS;
    advance(epoll_fd,p,now,operation);
}
void upstream_health_tick(int epoll_fd,int64_t now) {
    if(!config.upstream_health_enabled)return;
    uint32_t busy=0;
    for(unsigned i=0;i<PROBE_LIMIT;i++) {
        struct probe *p=&probes[i];
        if(p->socket && now>=p->deadline)advance(epoll_fd,p,now,EPOLL_CTL_MOD);
        if(p->socket)busy|=UINT32_C(1)<<p->backend;
    }
    for(unsigned i=0;i<PROBE_LIMIT;i++) {
        struct probe *p=&probes[i];
        if(p->socket)continue;
        unsigned checked=0;
        while(checked++<config.backend_count) {
            unsigned index=next_member;
            next_member=(next_member+1)%config.backend_count;
            struct health *h=&members[index];
            if((busy & (UINT32_C(1)<<index)) || now<h->next_probe)continue;
            h->next_probe=now+INTERVAL_MS;
            int error;
            upstream *socket=upstream_open(index,now,&error);
            if(!socket){if(error==502)probe_result(index,0,now);continue;}
            p->socket=socket;p->backend=index;p->deadline=now+PROBE_TIMEOUT_MS;
            p->ready=p->closing=0;p->sent=p->used=p->scan=p->info_bytes=p->informational=0;
            p->tag=UPSTREAM_HEALTH_EVENT|(sequence++<<16)|i;
            p->length=(size_t)snprintf(p->request,sizeof p->request,
                "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                config.upstream_health_path,config.backends[index].host);
            advance(epoll_fd,p,now,EPOLL_CTL_ADD);
            if(p->socket)busy|=UINT32_C(1)<<index;
            break;
        }
    }
}
void upstream_health_event(int epoll_fd,uint64_t tag,int64_t now) {
    unsigned index=(unsigned)(tag&65535);
    if(index<PROBE_LIMIT && probes[index].socket && probes[index].tag==tag)
        advance(epoll_fd,&probes[index],now,EPOLL_CTL_MOD);
}
void upstream_health_stop(void) {
    for(unsigned i=0;i<PROBE_LIMIT;i++)if(probes[i].socket)drop(&probes[i]);
}
