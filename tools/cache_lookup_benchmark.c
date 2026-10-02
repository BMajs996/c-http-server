#define _POSIX_C_SOURCE 200809L
#include "file_cache.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_seconds(void) {
    struct timespec time;
    if(clock_gettime(CLOCK_MONOTONIC,&time)<0)return -1;
    return (double)time.tv_sec+(double)time.tv_nsec/1e9;
}

int main(void) {
    config_defaults();
    config.cache_bytes=16777216;
    config.cache_entries=4096;
    config.cache_ttl_ms=60000;
    char path[64];
    for(int i=0;i<3000;++i) {
        snprintf(path,sizeof path,"/file-%04d",i);
        struct static_file file={.fd=-1,.length=1};
        file.data=malloc(2);
        if(!file.data)return 1;
        memcpy(file.data,"x",2);
        file_cache_entry *entry=file_cache_insert(path,&file);
        if(!entry){free(file.data);return 2;}
        file_cache_release(entry);
    }
    double start=now_seconds();
    if(start<0)return 3;
    for(int i=0;i<300000;++i) {
        snprintf(path,sizeof path,"/file-%04d",i%3000);
        file_cache_entry *entry=file_cache_get(path);
        if(!entry)return 4;
        file_cache_release(entry);
    }
    printf("300000 lookups in %.3f s\n",now_seconds()-start);
    file_cache_close();
    return 0;
}
