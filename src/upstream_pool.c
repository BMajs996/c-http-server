#include "upstream_pool.h"
#include "config.h"
#include "upstream_health.h"
#include <stdlib.h>
#include <sys/epoll.h>
#define CLOSE_TIMEOUT_MS 1000
enum entry_state { POOL_IDLE, POOL_CLOSING };
struct entry { upstream *connection; uint64_t tag;int64_t deadline;unsigned backend;enum entry_state state; };
static struct entry *entries;
static unsigned capacity;
static int reactor=-1,draining;
static uint64_t sequence=1;
static struct upstream_pool_stats stats, pools[BACKEND_LIMIT];
static uint64_t assigned[BACKEND_LIMIT];
static int weights[BACKEND_LIMIT];
static unsigned cursor;
static uint32_t weighted_members;
#define COUNT(index, field, delta) do { stats.field += (delta); pools[index].field += (delta); } while (0)
void upstream_pool_backend(unsigned index,struct upstream_pool_stats *out,uint64_t *requests) {
    *out=pools[index];*requests=assigned[index];
}
void upstream_pool_snapshot(struct upstream_pool_stats *out){*out=stats;}
static void unwatch(struct entry *entry) {
    if(reactor>=0)(void)epoll_ctl(reactor,EPOLL_CTL_DEL,upstream_state(entry->connection).fd,NULL);
    if(entry->state==POOL_IDLE)COUNT(entry->backend,idle,-1);else COUNT(entry->backend,closing,-1);
    entry->tag=0;
}
static void destroy(unsigned index,upstream *u) {
    upstream_destroy(u);COUNT(index,total,-1);COUNT(index,discarded,1);
}
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
static void close_connection(unsigned index,upstream *u,int64_t now) {
    uint32_t wanted=0;
    if(upstream_shutdown(u,&wanted)!=0){destroy(index,u);return;}
    struct entry *entry=free_entry();
    if(!entry){destroy(index,u);return;}
    *entry=(struct entry){.connection=u,.deadline=now+CLOSE_TIMEOUT_MS,.backend=index,.state=POOL_CLOSING};
    COUNT(index,closing,1);
    if(watch(entry,wanted)<0){unwatch(entry);entry->connection=NULL;destroy(entry->backend,u);}
}
static int expired(upstream *u,int64_t now) {
    struct upstream_state state=upstream_state(u);
    return now-state.born>=config.upstream_pool_max_age_ms || state.requests>=config.upstream_pool_max_requests;
}
int upstream_pool_init(int epoll_fd) {
    reactor=epoll_fd;draining=0;
    capacity=config.upstream_pool_enabled?config.upstream_pool_max_connections:0;
    if(!capacity)for(unsigned i=0;i<config.backend_count;i++) {
        if(config.backends[i].tls){capacity=config.max_connections;break;}
    }
    if(capacity){entries=calloc(capacity,sizeof *entries);if(!entries){capacity=0;reactor=-1;return -1;}}
    return 0;
}
static unsigned open_limit(void) {
    return config.upstream_pool_enabled?config.upstream_pool_max_connections:config.max_connections;
}
static unsigned member_limit(unsigned index) {
    unsigned limit=config.backends[index].pool_limit;
    return limit?limit:open_limit();
}
static upstream *take_idle(unsigned index,int64_t now) {
    for(unsigned i=0;i<capacity;i++) {
        struct entry *entry=&entries[i];
        if(!entry->connection || entry->state!=POOL_IDLE || entry->backend!=index)continue;
        upstream *u=entry->connection;uint32_t wanted=0;
        int old=now>=entry->deadline || expired(u,now);
        unwatch(entry);entry->connection=NULL;
        if(!old && upstream_clean(u,&wanted) && !(wanted&EPOLLOUT)) {
            COUNT(index,reused,1);return u;
        }
        if(old)COUNT(index,expired,1);
        close_connection(index,u,now);
    }
    return NULL;
}
static void discard_idle(struct entry *entry,int64_t now) {
    upstream *u=entry->connection;
    unwatch(entry);entry->connection=NULL;
    close_connection(entry->backend,u,now);
}
void upstream_pool_drop_idle(unsigned index,int64_t now) {
    for(unsigned i=0;i<capacity;i++)
        if(entries[i].connection && entries[i].state==POOL_IDLE && entries[i].backend==index)
            discard_idle(&entries[i],now);
}
static void reclaim_idle(unsigned index,int64_t now) {
    struct entry *oldest=NULL;
    for(unsigned i=0;i<capacity;i++) {
        struct entry *entry=&entries[i];
        if(entry->connection && entry->state==POOL_IDLE && entry->backend!=index &&
           (!oldest || entry->deadline<oldest->deadline))oldest=entry;
    }
    if(oldest)discard_idle(oldest,now);
}
static unsigned choose(uint32_t eligible) {
    if(config.upstream_balance==WEIGHTED_ROUND_ROBIN && eligible!=weighted_members) {
        /* Start a fresh weighted cycle when membership changes. */
        for(unsigned i=0;i<config.backend_count;i++)weights[i]=0;
        weighted_members=eligible;
    }
    unsigned best=BACKEND_LIMIT;
    for(unsigned offset=0;offset<config.backend_count;offset++) {
        unsigned i=(cursor+offset)%config.backend_count;
        if(!(eligible & (UINT32_C(1)<<i)))continue;
        if(best==BACKEND_LIMIT ||
           (config.upstream_balance==LEAST_CONNECTIONS && pools[i].leased<pools[best].leased) ||
           (config.upstream_balance==WEIGHTED_ROUND_ROBIN &&
            weights[i]+(int)config.backends[i].weight>weights[best]+(int)config.backends[best].weight))best=i;
    }
    return best;
}
static void record_selection(unsigned index,uint32_t eligible) {
    assigned[index]++;
    cursor=(index+1)%config.backend_count;
    if(config.upstream_balance!=WEIGHTED_ROUND_ROBIN)return;
    int total=0;
    for(unsigned i=0;i<config.backend_count;i++) {
        if(eligible & (UINT32_C(1)<<i)) {
            weights[i]+=(int)config.backends[i].weight;
            total+=(int)config.backends[i].weight;
        } else weights[i]=0;
    }
    weights[index]-=total;
}
upstream *upstream_acquire(int64_t now,unsigned *index,int *error) {
    *error=503;
    if(draining)return NULL;
    uint32_t eligible=0;
    int healthy=0;
    for(unsigned i=0;i<config.backend_count;i++) {
        if(!upstream_healthy(i,now))continue;
        healthy=1;
        if(pools[i].idle || (pools[i].total<member_limit(i) && (stats.total<open_limit() || stats.idle)))
            eligible|=UINT32_C(1)<<i;
    }
    int reclaimed=0;
    while(eligible) {
        unsigned i=choose(eligible);
        upstream *u=pools[i].idle?take_idle(i,now):NULL;
        if(!u) {
            if(!reclaimed && pools[i].total<member_limit(i) && stats.total>=open_limit() && stats.idle) {
                reclaim_idle(i,now);reclaimed=1;
            }
            if(pools[i].total>=member_limit(i) || stats.total>=open_limit()) {
                eligible&=~(UINT32_C(1)<<i);continue;
            }
            *index=i;record_selection(i,eligible);
            u=upstream_open(i,now,error);
            if(!u)return NULL; /* A connection attempt pins the request to this member. */
            COUNT(i,total,1);COUNT(i,opened,1);
        } else {
            *index=i;record_selection(i,eligible);*error=0;
        }
        upstream_begin(u,now);COUNT(i,leased,1);return u;
    }
    if(healthy)stats.exhaustions++;
    return NULL;
}
void upstream_release(unsigned index,upstream *u,int reusable,int64_t now) {
    if(!u)return;
    COUNT(index,leased,-1);
    uint32_t wanted=0;
    if(config.upstream_pool_enabled && reusable && !draining && upstream_healthy(index,now) &&
       stats.idle<config.upstream_pool_idle_limit && !expired(u,now) && upstream_clean(u,&wanted)) {
        struct entry *entry=free_entry();
        if(entry) {
            *entry=(struct entry){.connection=u,.deadline=now+config.upstream_pool_idle_timeout_ms,.backend=index,.state=POOL_IDLE};
            COUNT(index,idle,1);
            if(watch(entry,wanted)==0)return;
            unwatch(entry);entry->connection=NULL;
        }
    }
    close_connection(index,u,now);
}
void upstream_pool_event(uint64_t tag,int64_t now) {
    unsigned index=(unsigned)(tag&65535);
    if(index>=capacity || entries[index].tag!=tag || !entries[index].connection)return;
    struct entry *entry=&entries[index];upstream *u=entry->connection;uint32_t wanted=0;
    int keep=entry->state==POOL_IDLE?upstream_clean(u,&wanted):upstream_shutdown(u,&wanted)==0;
    if(keep && now<entry->deadline && watch(entry,wanted)==0)return;
    unwatch(entry);entry->connection=NULL;destroy(entry->backend,u);
}
void upstream_pool_tick(int64_t now) {
    for(unsigned i=0;i<capacity;i++)if(entries[i].connection && (now>=entries[i].deadline || (entries[i].state==POOL_IDLE && expired(entries[i].connection,now)))) {
        upstream *u=entries[i].connection;int idle=entries[i].state==POOL_IDLE;
        unsigned index=entries[i].backend;
        unwatch(&entries[i]);entries[i].connection=NULL;COUNT(index,expired,1);
        if(idle)close_connection(index,u,now);else destroy(index,u);
    }
}
void upstream_pool_drain(int64_t now) {
    draining=1;
    for(unsigned i=0;i<capacity;i++)if(entries[i].connection && entries[i].state==POOL_IDLE) {
        discard_idle(&entries[i],now);
    }
}
int upstream_pool_pending(void){return stats.closing!=0;}
void upstream_pool_close(void) {
    for(unsigned i=0;i<capacity;i++)if(entries[i].connection){unwatch(&entries[i]);destroy(entries[i].backend,entries[i].connection);}
    free(entries);entries=NULL;capacity=0;reactor=-1;draining=0;
}
