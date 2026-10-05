#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
server_config config;
void config_defaults(void) {
    config = (server_config){.port=8080,.max_connections=1024,.shutdown_ms=5000,
        .request_timeout_ms=5000,.response_timeout_ms=30000,.max_requests=100,
        .file_workers=4,.file_job_limit=64,.log_queue_limit=1024,
        .auth_timestamp_window_s=60,.auth_nonce_entries=4096,
        .cache_max_file_bytes=65536,.cache_entries=256,.cache_ttl_ms=1000};
    strcpy(config.document_root,"public");strcpy(config.access_log,"-");
}
static char *trim(char *value) {
    while (*value==' ' || *value=='\t') ++value;
    char *end=value+strlen(value);
    while(end>value && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r' || end[-1]=='\n')) *--end=0;
    return value;
}
int config_load(const char *path) {
    struct option { const char *name; unsigned *value; unsigned low, high; } options[] = {
        {"port",&config.port,1,65535},{"max_connections",&config.max_connections,1,16384},
        {"shutdown_ms",&config.shutdown_ms,1,60000},{"request_timeout_ms",&config.request_timeout_ms,1,300000},
        {"response_timeout_ms",&config.response_timeout_ms,1,300000},{"max_requests",&config.max_requests,1,100000},
        {"file_workers",&config.file_workers,1,16},{"file_job_limit",&config.file_job_limit,1,1024},
        {"log_queue_limit",&config.log_queue_limit,1,8192},{"cache_entries",&config.cache_entries,1,4096},
        {"cache_ttl_ms",&config.cache_ttl_ms,1,60000},
        {"auth_timestamp_window_s",&config.auth_timestamp_window_s,1,300},
        {"auth_nonce_entries",&config.auth_nonce_entries,1,65536}
    };
    struct text_option { const char *name; char *value; size_t size; } texts[]={
        {"auth_credentials_file",config.auth_credentials_file,sizeof config.auth_credentials_file},
        {"document_root",config.document_root,sizeof config.document_root},
        {"access_log",config.access_log,sizeof config.access_log},
        {"tls_cert",config.tls_cert,sizeof config.tls_cert},{"tls_key",config.tls_key,sizeof config.tls_key}
    };
    FILE *file=fopen(path,"r");if(!file){perror("configuration");return -1;}
    char line[4096], keys[32][64];unsigned line_no=0,key_count=0;
    while(fgets(line,sizeof line,file)) {
        ++line_no;
        if(!strchr(line,'\n') && !feof(file)) goto invalid;
        char *key=trim(line);if(!*key || *key=='#')continue;
        char *equal=strchr(key,'=');if(!equal)goto invalid;
        *equal=0;key=trim(key);char *value=trim(equal+1);
        if(!*key || strlen(key)>=64 || key_count>=32)goto invalid;
        for(unsigned i=0;i<key_count;++i)if(!strcmp(keys[i],key))goto invalid;
        strcpy(keys[key_count++],key);
        int found=0;
        for(size_t i=0;i<sizeof texts/sizeof texts[0];++i)if(!strcmp(key,texts[i].name)) {
            if(strlen(value)>=texts[i].size || (!*value && strncmp(key,"tls_",4) && strcmp(key,"auth_credentials_file")))goto invalid;
            strcpy(texts[i].value,value);found=1;break;
        }
        if(found)continue;
        if(!*value || *value<'0' || *value>'9')goto invalid;
        errno=0;char *end;unsigned long long n=strtoull(value,&end,10);
        if(errno || *end)goto invalid;
        if(!strcmp(key,"cache_bytes")) {
            if(n>268435456ULL)goto invalid;
            config.cache_bytes=(size_t)n;continue;
        }
        if(!strcmp(key,"cache_max_file_bytes")) {
            if(n<1 || n>1048576ULL)goto invalid;
            config.cache_max_file_bytes=(size_t)n;continue;
        }
        for(size_t i=0;i<sizeof options/sizeof options[0];++i)if(!strcmp(key,options[i].name)) {
            if(n<options[i].low || n>options[i].high)goto invalid;
            *options[i].value=(unsigned)n;found=1;break;
        }
        if(!found)goto invalid;
    }
    if(ferror(file))goto invalid;
    fclose(file);
    if((!*config.tls_cert)!=(!*config.tls_key)) {
        fprintf(stderr,"configuration: tls_cert and tls_key must be provided together\n");return -1;
    }
    return 0;
invalid:
    fprintf(stderr,"configuration: invalid, duplicate, unknown, or out-of-range setting at %s:%u\n",path,line_no);
    fclose(file);return -1;
}
