#define _POSIX_C_SOURCE 200809L
#include "file_cache.h"
#include "config.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
static struct static_file file(const char *text) {
    struct static_file f={.fd=-1,.length=(off_t)strlen(text),.type="text/plain"};
    f.data=malloc(strlen(text)+1);assert(f.data);strcpy(f.data,text);return f;
}
int main(void) {
    config_defaults();config.cache_bytes=8192;config.cache_entries=1;config.cache_ttl_ms=1;
    struct static_file a=file("immutable bytes"),b=file("replacement bytes");
    file_cache_entry *pinned=file_cache_insert("/a",&a);assert(pinned && !a.data);
    /* A pinned cache entry cannot be evicted to admit another file. */
    assert(!file_cache_insert("/b",&b) && b.data);
    assert(!strcmp(file_cache_data(pinned),"immutable bytes"));
    struct file_cache_stats stats;file_cache_snapshot(&stats);
    assert(stats.entries==1 && stats.bytes<=config.cache_bytes && stats.bypasses==1);
    struct timespec delay={0,5000000};nanosleep(&delay,NULL);
    /* Expiry retires it, but the response reference keeps storage alive. */
    assert(!file_cache_get("/a"));
    assert(!strcmp(file_cache_data(pinned),"immutable bytes"));
    file_cache_release(pinned);file_cache_snapshot(&stats);assert(stats.entries==0 && stats.bytes==0);
    file_cache_entry *new_entry=file_cache_insert("/b",&b);assert(new_entry && !b.data);
    file_cache_release(new_entry);file_cache_close();
    file_cache_snapshot(&stats);assert(!stats.entries && !stats.bytes);
    /* Disable cache cleanly. */
    config.cache_bytes=0;assert(!file_cache_get("/b"));
    puts("Cache pinning, capacity, retirement, and ownership checks passed.");
    return 0;
}
