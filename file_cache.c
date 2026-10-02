#define _POSIX_C_SOURCE 200809L
#include "file_cache.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct file_cache_entry {
    struct file_cache_entry *next;
    struct static_file file;
    char path[2048];
    unsigned references;
    int valid;
    int64_t expires;uint64_t touched;
    size_t cost;
};
static file_cache_entry *entries;
static struct file_cache_stats stats;
static uint64_t clock_sequence;
static int64_t now_ms(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)<0)return 0;
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void remove_entry(file_cache_entry *entry) {
    file_cache_entry **p=&entries;
    while(*p && *p!=entry)p=&(*p)->next;
    if(!*p)return;
    *p=entry->next;stats.bytes-=entry->cost;--stats.entries;
    free(entry->file.data);free(entry);
}
file_cache_entry *file_cache_get(const char *path) {
    if(!config.cache_bytes)return NULL;
    int64_t now=now_ms();
    for(file_cache_entry *e=entries;e;e=e->next) {
        if(e->valid && !strcmp(e->path,path)) {
            if(now>=e->expires) {
                e->valid=0;++stats.expirations;
                if(!e->references)remove_entry(e);
                break;
            }
            ++e->references;e->touched=++clock_sequence;++stats.hits;return e;
        }
    }
    ++stats.misses;return NULL;
}
file_cache_entry *file_cache_insert(const char *path,struct static_file *file) {
    if(!config.cache_bytes || !file->data)return NULL;
    size_t cost=sizeof(file_cache_entry)+(size_t)file->length+1;
    if(cost>config.cache_bytes){++stats.bypasses;return NULL;}
    /* Retire a superseded concurrent load, but keep its pinned bytes alive. */
    for(file_cache_entry *e=entries,*next;e;e=next) {
        next=e->next;
        if(e->valid && !strcmp(e->path,path)) {
            e->valid=0;if(!e->references)remove_entry(e);
        }
    }
    while(stats.bytes+cost>config.cache_bytes || stats.entries>=config.cache_entries) {
        file_cache_entry *victim=NULL;
        for(file_cache_entry *e=entries;e;e=e->next)
            if(!e->references && (!victim || e->touched<victim->touched))victim=e;
        if(!victim){++stats.bypasses;return NULL;}
        remove_entry(victim);++stats.evictions;
    }
    file_cache_entry *e=calloc(1,sizeof *e);
    if(!e){++stats.bypasses;return NULL;}
    e->file=*file;e->file.fd=-1;file->data=NULL;
    strcpy(e->path,path);e->references=1;e->valid=1;e->cost=cost;
    e->touched=++clock_sequence;e->expires=now_ms()+config.cache_ttl_ms;
    e->next=entries;entries=e;stats.bytes+=cost;++stats.entries;return e;
}
struct static_file file_cache_metadata(const file_cache_entry *entry) {
    struct static_file result=entry->file;result.fd=-1;result.data=NULL;return result;
}
const char *file_cache_data(const file_cache_entry *entry){return entry->file.data;}
void file_cache_release(file_cache_entry *entry) {
    if(!entry)return;
    if(entry->references)--entry->references;
    if(!entry->references && !entry->valid)remove_entry(entry);
}
void file_cache_snapshot(struct file_cache_stats *result){*result=stats;}
void file_cache_close(void) {
    while(entries)remove_entry(entries);
}
