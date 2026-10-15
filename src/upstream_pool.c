#include "upstream_pool.h"
#include "config.h"
#include <stdlib.h>
#include <sys/epoll.h>
#define CLOSE_TIMEOUT_MS 1000
enum entry_state { POOL_IDLE, POOL_CLOSING };
struct entry { upstream *connection; uint64_t tag;int64_t deadline;enum entry_state state; };
static struct entry *entries;
static unsigned capacity;
static int reactor=-1,draining;
static uint64_t sequence=1;
static struct upstream_pool_stats stats;
void upstream_pool_snapshot(struct upstream_pool_stats *out){*out=stats;}
static void unwatch(struct entry *entry) {
    if(reactor>=0)(void)epoll_ctl(reactor,EPOLL_CTL_DEL,upstream_state(entry->connection).fd,NULL);
    if(entry->state==POOL_IDLE)stats.idle--;else stats.closing--;
    entry->tag=0;
}
static void destroy(upstream *u){upstream_destroy(u);stats.total--;stats.discarded++;}
static int watch(struct entry *entry,uint32_t wanted) {
    if(reactor<0)return -1;
    unsigned index=(unsigned)(entry-entries);
    entry->tag=UPSTREAM_POOL_EVENT|(sequence++<<16)|index;
    struct epoll_event event={.events=wanted|EPOLLRDHUP,.data.u64=entry->tag};
    int fd=upstream_state(entry->connection).fd;
    (void)epoll_ctl(reactor,EPOLL_CTL_DEL,fd,NULL);
    return epoll_ctl(reactor,EPOLL_CTL_ADD,fd,&event);
}
static struct entry *free_entry(void) {
    for(unsigned i=0;i<capacity;i++)if(!entries[i].connection)return &entries[i];
    return NULL;
}
static void close_connection(upstream *u,int64_t now) {
    uint32_t wanted=0;
    if(upstream_shutdown(u,&wanted)!=0){destroy(u);return;}
    struct entry *entry=free_entry();
    if(!entry){destroy(u);return;}
    *entry=(struct entry){.connection=u,.deadline=now+CLOSE_TIMEOUT_MS,.state=POOL_CLOSING};
    stats.closing++;
    if(watch(entry,wanted)<0){unwatch(entry);entry->connection=NULL;destroy(u);}
}
static int expired(upstream *u,int64_t now) {
    struct upstream_state state=upstream_state(u);
    return now-state.born>=config.upstream_pool_max_age_ms || state.requests>=config.upstream_pool_max_requests;
}
int upstream_pool_init(int epoll_fd) {
    reactor=epoll_fd;draining=0;
    capacity=config.upstream_pool_enabled?config.upstream_pool_max_connections:config.upstream_tls?config.max_connections:0;
    if(capacity){entries=calloc(capacity,sizeof *entries);if(!entries){capacity=0;reactor=-1;return -1;}}
    return 0;
}
upstream *upstream_acquire(int64_t now,int *error) {
    if(draining){*error=503;return NULL;}
    for(unsigned i=0;i<capacity;i++)if(entries[i].connection && entries[i].state==POOL_IDLE) {
        struct entry *entry=&entries[i];upstream *u=entry->connection;uint32_t wanted=0;
        int old=now>=entry->deadline || expired(u,now);
        unwatch(entry);entry->connection=NULL;
        if(!old && upstream_clean(u,&wanted) && !(wanted&EPOLLOUT)) {
            upstream_begin(u,now);stats.leased++;stats.reused++;*error=0;return u;
        }
        if(old)stats.expired++;
        close_connection(u,now);
    }
    unsigned limit=config.upstream_pool_enabled?config.upstream_pool_max_connections:config.max_connections;
    if(stats.total>=limit){stats.exhaustions++;*error=503;return NULL;}
    upstream *u=upstream_open(now,error);
    if(u){upstream_begin(u,now);stats.total++;stats.leased++;stats.opened++;}
    return u;
}
void upstream_release(upstream *u,int reusable,int64_t now) {
    if(!u)return;
    stats.leased--;
    uint32_t wanted=0;
    if(config.upstream_pool_enabled && reusable && !draining && stats.idle<config.upstream_pool_idle_limit && !expired(u,now) && upstream_clean(u,&wanted)) {
        struct entry *entry=free_entry();
        if(entry) {
            *entry=(struct entry){.connection=u,.deadline=now+config.upstream_pool_idle_timeout_ms,.state=POOL_IDLE};
            stats.idle++;
            if(watch(entry,wanted)==0)return;
            unwatch(entry);entry->connection=NULL;
        }
    }
    close_connection(u,now);
}
void upstream_pool_event(uint64_t tag,int64_t now) {
    unsigned index=(unsigned)(tag&65535);
    if(index>=capacity || entries[index].tag!=tag || !entries[index].connection)return;
    struct entry *entry=&entries[index];upstream *u=entry->connection;uint32_t wanted=0;
    int keep=entry->state==POOL_IDLE?upstream_clean(u,&wanted):upstream_shutdown(u,&wanted)==0;
    if(keep && now<entry->deadline && watch(entry,wanted)==0)return;
    unwatch(entry);entry->connection=NULL;destroy(u);
}
void upstream_pool_tick(int64_t now) {
    for(unsigned i=0;i<capacity;i++)if(entries[i].connection && (now>=entries[i].deadline || (entries[i].state==POOL_IDLE && expired(entries[i].connection,now)))) {
        upstream *u=entries[i].connection;int idle=entries[i].state==POOL_IDLE;
        unwatch(&entries[i]);entries[i].connection=NULL;stats.expired++;
        if(idle)close_connection(u,now);else destroy(u);
    }
}
void upstream_pool_drain(int64_t now) {
    draining=1;
    for(unsigned i=0;i<capacity;i++)if(entries[i].connection && entries[i].state==POOL_IDLE) {
        upstream *u=entries[i].connection;unwatch(&entries[i]);entries[i].connection=NULL;close_connection(u,now);
    }
}
int upstream_pool_pending(void){return stats.closing!=0;}
void upstream_pool_close(void) {
    for(unsigned i=0;i<capacity;i++)if(entries[i].connection){unwatch(&entries[i]);destroy(entries[i].connection);}
    free(entries);entries=NULL;capacity=0;reactor=-1;draining=0;
}
