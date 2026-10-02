#define _POSIX_C_SOURCE 200809L
#include "file_cache.h"
#include "config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct static_file file(const char *text) {
    struct static_file result={.fd=-1,.length=(off_t)strlen(text),.type="text/plain"};
    result.data=malloc(strlen(text)+1);assert(result.data);
    strcpy(result.data,text);return result;
}
static file_cache_entry *insert(const char *path,const char *text) {
    struct static_file value=file(text);
    file_cache_entry *entry=file_cache_insert(path,&value);
    assert(entry && !value.data);return entry;
}
static void assert_absent(const char *path) {
    file_cache_entry *entry=file_cache_get(path);
    assert(!entry);
}
static void assert_present(const char *path,const char *text) {
    file_cache_entry *entry=file_cache_get(path);
    assert(entry && !strcmp(file_cache_data(entry),text));
    file_cache_release(entry);
}
static void assert_empty(void) {
    struct file_cache_stats current;file_cache_snapshot(&current);
    assert(!current.entries && !current.bytes);
}
int main(void) {
    config_defaults();config.cache_bytes=8192;config.cache_entries=1;config.cache_ttl_ms=1;
    struct static_file first=file("immutable bytes"),second=file("replacement bytes");
    file_cache_entry *pinned=file_cache_insert("/a",&first);assert(pinned && !first.data);
    /* A pinned cache entry cannot be evicted to admit another file. */
    assert(!file_cache_insert("/b",&second) && second.data);
    assert(!strcmp(file_cache_data(pinned),"immutable bytes"));
    struct file_cache_stats current;file_cache_snapshot(&current);
    assert(current.entries==1 && current.bytes<=config.cache_bytes && current.bypasses==1);
    struct timespec delay={0,5000000};nanosleep(&delay,NULL);
    /* Expiry removes lookup visibility, while the response keeps storage. */
    assert_absent("/a");assert_absent("/a");
    file_cache_snapshot(&current);assert(current.expirations==1 && current.entries==1);
    assert(!strcmp(file_cache_data(pinned),"immutable bytes"));
    file_cache_release(pinned);assert_empty();
    file_cache_entry *replacement=file_cache_insert("/b",&second);
    assert(replacement && !second.data);file_cache_release(replacement);
    file_cache_close();assert_empty();

    /* Lookup across many hash buckets and collisions; a hit updates LRU. */
    config.cache_entries=256;config.cache_bytes=1024*1024;config.cache_ttl_ms=60000;
    for(int i=0;i<200;++i) {
        char path[32],text[32];
        snprintf(path,sizeof path,"/file-%03d",i);
        snprintf(text,sizeof text,"content-%03d",i);
        file_cache_release(insert(path,text));
    }
    for(int i=199;i>=0;--i) {
        char path[32],text[32];
        snprintf(path,sizeof path,"/file-%03d",i);
        snprintf(text,sizeof text,"content-%03d",i);
        assert_present(path,text);
    }
    file_cache_close();assert_empty();

    /* These names share a bucket when the two-entry table has four buckets. */
    config.cache_entries=2;
    file_cache_release(insert("/collision-0","first"));
    file_cache_release(insert("/collision-4","second"));
    assert_present("/collision-0","first");
    assert_present("/collision-4","second");
    file_cache_release(insert("/collision-0","replaced"));
    assert_present("/collision-4","second");
    assert_present("/collision-0","replaced");
    file_cache_close();assert_empty();

    config.cache_entries=3;
    file_cache_release(insert("/a","a"));
    file_cache_release(insert("/b","b"));
    file_cache_release(insert("/c","c"));
    assert_present("/a","a");
    file_cache_release(insert("/d","d"));
    assert_absent("/b");assert_present("/a","a");
    assert_present("/c","c");assert_present("/d","d");
    file_cache_close();assert_empty();

    /* A pinned LRU tail is skipped; a retired entry still consumes capacity. */
    config.cache_entries=2;
    pinned=insert("/old","old");
    file_cache_release(insert("/free","free"));
    file_cache_release(insert("/new","new"));
    assert(!strcmp(file_cache_data(pinned),"old"));
    assert_absent("/free");
    file_cache_release(pinned);
    pinned=file_cache_get("/old");assert(pinned);
    replacement=insert("/old","replacement");
    assert_absent("/new"); /* The second slot is needed for the retired old entry. */
    assert(!strcmp(file_cache_data(pinned),"old"));
    assert_present("/old","replacement");
    file_cache_release(replacement);file_cache_release(pinned);
    file_cache_close();assert_empty();

    /* Failed insertion keeps caller ownership, including oversized paths. */
    config.cache_entries=1;config.cache_bytes=512;
    char long_path[2050];memset(long_path,'x',sizeof long_path-1);long_path[0]='/';long_path[sizeof long_path-1]=0;
    struct static_file value=file("x");
    assert(!file_cache_insert(long_path,&value) && value.data);free(value.data);
    config.cache_bytes=0;assert_absent("/old");
    puts("Cache lookup, LRU, pinning, capacity, expiry, and ownership checks passed.");
    return 0;
}
