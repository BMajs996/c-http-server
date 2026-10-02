#define _POSIX_C_SOURCE 200809L
#include "file_cache.h"
#include "config.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CACHE_PATH_CAP 2048

struct file_cache_entry {
    struct file_cache_entry *all_prev, *all_next;
    struct file_cache_entry *lru_prev, *lru_next;
    struct file_cache_entry *hash_next, **hash_prev_next;
    off_t length;
    const char *type;
    char etag[160], last_modified[64];
    char *data;
    unsigned references;
    int valid;
    int64_t expires;
    size_t cost;
    char path[];
};

/* Only valid entries appear in buckets and LRU. All entries, including retired
 * response-pinned entries, remain in the ownership list until freed. */
static file_cache_entry *all_entries, *lru_first, *lru_last;
static file_cache_entry **buckets;
static size_t bucket_count;
static struct file_cache_stats stats;

static int64_t now_ms(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)<0)return 0;
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}

static size_t bucket_for(const char *path) {
    uint64_t hash=UINT64_C(14695981039346656037);
    for(const unsigned char *p=(const unsigned char *)path;*p;++p) {
        hash^=*p;hash*=UINT64_C(1099511628211);
    }
    return (size_t)hash&(bucket_count-1);
}

static int ensure_buckets(void) {
    if(buckets)return 1;
    /* The configured entry limit is at most 4096. The auxiliary table has at
     * most 8192 pointers and is tracked separately from cache entry bytes. */
    size_t count=2;
    while(count<(size_t)config.cache_entries*2)count*=2;
    buckets=calloc(count,sizeof *buckets);
    if(!buckets)return 0;
    bucket_count=count;
    return 1;
}

static void lru_unlink(file_cache_entry *entry) {
    if(entry->lru_prev)entry->lru_prev->lru_next=entry->lru_next;
    else lru_first=entry->lru_next;
    if(entry->lru_next)entry->lru_next->lru_prev=entry->lru_prev;
    else lru_last=entry->lru_prev;
    entry->lru_prev=entry->lru_next=NULL;
}

static void lru_front(file_cache_entry *entry) {
    entry->lru_prev=NULL;entry->lru_next=lru_first;
    if(lru_first)lru_first->lru_prev=entry;
    else lru_last=entry;
    lru_first=entry;
}

static void hash_unlink(file_cache_entry *entry) {
    *entry->hash_prev_next=entry->hash_next;
    if(entry->hash_next)entry->hash_next->hash_prev_next=entry->hash_prev_next;
    entry->hash_next=NULL;entry->hash_prev_next=NULL;
}

static void hash_insert(file_cache_entry *entry) {
    file_cache_entry **head=&buckets[bucket_for(entry->path)];
    entry->hash_next=*head;
    entry->hash_prev_next=head;
    if(*head)(*head)->hash_prev_next=&entry->hash_next;
    *head=entry;
}

static file_cache_entry *find_valid(const char *path) {
    if(!buckets)return NULL;
    for(file_cache_entry *entry=buckets[bucket_for(path)];entry;entry=entry->hash_next)
        if(!strcmp(entry->path,path))return entry;
    return NULL;
}

static void remove_entry(file_cache_entry *entry) {
    if(entry->valid){hash_unlink(entry);lru_unlink(entry);}
    if(entry->all_prev)entry->all_prev->all_next=entry->all_next;
    else all_entries=entry->all_next;
    if(entry->all_next)entry->all_next->all_prev=entry->all_prev;
    stats.bytes-=entry->cost;--stats.entries;
    free(entry->data);free(entry);
}

static void retire_entry(file_cache_entry *entry) {
    hash_unlink(entry);lru_unlink(entry);entry->valid=0;
    if(!entry->references)remove_entry(entry);
}

file_cache_entry *file_cache_get(const char *path) {
    if(!config.cache_bytes)return NULL;
    file_cache_entry *entry=find_valid(path);
    if(entry) {
        if(now_ms()>=entry->expires) {
            ++stats.expirations;retire_entry(entry);
        } else {
            ++entry->references;lru_unlink(entry);lru_front(entry);
            ++stats.hits;return entry;
        }
    }
    ++stats.misses;return NULL;
}

file_cache_entry *file_cache_insert(const char *path,struct static_file *file) {
    if(!config.cache_bytes || !file->data)return NULL;
    size_t length=strlen(path);
    if(length>=CACHE_PATH_CAP || file->length<0 ||
       (uintmax_t)file->length>SIZE_MAX-sizeof(file_cache_entry)-length-2) {
        ++stats.bypasses;return NULL;
    }
    size_t allocation_size=sizeof(file_cache_entry)+length+1;
    size_t cost=allocation_size+(size_t)file->length+1;
    if(cost>config.cache_bytes || !ensure_buckets()) {
        ++stats.bypasses;return NULL;
    }
    /* The latest completed load replaces the visible entry. A pinned old
     * entry remains owned and counted, but is no longer discoverable. */
    file_cache_entry *old=find_valid(path);
    if(old)retire_entry(old);
    while(stats.bytes>config.cache_bytes-cost || stats.entries>=config.cache_entries) {
        file_cache_entry *victim=lru_last;
        while(victim && victim->references)victim=victim->lru_prev;
        if(!victim){++stats.bypasses;return NULL;}
        remove_entry(victim);++stats.evictions;
    }
    file_cache_entry *entry=calloc(1,allocation_size);
    if(!entry){++stats.bypasses;return NULL;}
    entry->length=file->length;entry->type=file->type;
    strcpy(entry->etag,file->etag);
    strcpy(entry->last_modified,file->last_modified);
    entry->data=file->data;file->data=NULL;
    memcpy(entry->path,path,length+1);
    entry->references=1;entry->valid=1;entry->cost=cost;
    entry->expires=now_ms()+config.cache_ttl_ms;
    entry->all_next=all_entries;
    if(all_entries)all_entries->all_prev=entry;
    all_entries=entry;
    hash_insert(entry);lru_front(entry);
    stats.bytes+=cost;++stats.entries;
    return entry;
}

struct static_file file_cache_metadata(const file_cache_entry *entry) {
    /* stamp is used by the disk worker before insertion; cached responses
     * only need the representation metadata below. */
    struct static_file result={.fd=-1,.length=entry->length,.type=entry->type};
    strcpy(result.etag,entry->etag);
    strcpy(result.last_modified,entry->last_modified);
    return result;
}
const char *file_cache_data(const file_cache_entry *entry){return entry->data;}
void file_cache_release(file_cache_entry *entry) {
    if(!entry)return;
    if(entry->references)--entry->references;
    if(!entry->references && !entry->valid)remove_entry(entry);
}
void file_cache_snapshot(struct file_cache_stats *result){*result=stats;}
void file_cache_close(void) {
    while(all_entries)remove_entry(all_entries);
    free(buckets);buckets=NULL;bucket_count=0;
    lru_first=lru_last=NULL;
}
