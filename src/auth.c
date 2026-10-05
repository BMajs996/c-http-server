#define _XOPEN_SOURCE 700
#include "auth.h"
#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char id[AUTH_ID_LIMIT+1];
    auth_policy policy;
    unsigned char secret[32]; /* Bearer digest or signing key. */
} credential;
struct auth_keyset { unsigned count; credential entries[AUTH_CREDENTIAL_LIMIT]; };
typedef struct { unsigned char key[32]; int64_t expires, valid_until; unsigned next; } nonce_entry;
static auth_keyset *active;
static nonce_entry *nonces;
static unsigned *buckets, bucket_count, head, count;
static unsigned char hash_seed[32];
static int64_t wall_highwater, mono_highwater;
static struct auth_stats stats;
#define NONE UINT_MAX

static int valid_id(const char *id) {
    size_t length=strlen(id);
    if(!length || length>AUTH_ID_LIMIT)return 0;
    for(const unsigned char *p=(const unsigned char *)id;*p;++p)
        if(!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') ||
             (*p>='0' && *p<='9') || *p=='_' || *p=='-'))return 0;
    return 1;
}
static int unhex(const char *text,unsigned char *out,size_t length) {
    if(strlen(text)!=length*2)return 0;
    for(size_t i=0;i<length*2;++i) {
        unsigned char c=(unsigned char)text[i];unsigned digit;
        if(c>='0' && c<='9')digit=c-'0';
        else if(c>='a' && c<='f')digit=c-'a'+10;
        else return 0; /* Canonical lowercase only. */
        if(!(i&1))out[i/2]=(unsigned char)(digit<<4);else out[i/2]|=(unsigned char)digit;
    }
    return 1;
}
static void hex(const unsigned char *data,size_t length,char *out) {
    static const char digits[]="0123456789abcdef";
    for(size_t i=0;i<length;++i){out[2*i]=digits[data[i]>>4];out[2*i+1]=digits[data[i]&15];}
    out[2*length]=0;
}
static int digest(const void *data,size_t length,unsigned char out[32]) {
    size_t n=0;return EVP_Q_digest(NULL,"SHA256",NULL,data,length,out,&n)==1 && n==32;
}
void auth_keyset_free(auth_keyset *set) {
    if(set){OPENSSL_cleanse(set,sizeof *set);free(set);}
}
auth_keyset *auth_load(const char *path,const char *document_root) {
    char resolved[PATH_MAX],root[PATH_MAX];
    if(!realpath(path,resolved) || !realpath(document_root,root))return NULL;
    size_t n=strlen(root);
    if(!strcmp(root,"/") || (!strncmp(root,resolved,n) && (resolved[n]=='/' || !resolved[n])))return NULL;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return NULL;
    struct stat before,canonical,after;
    if(fstat(fd,&before)<0 || stat(resolved,&canonical)<0 || !S_ISREG(before.st_mode) ||
       before.st_dev!=canonical.st_dev || before.st_ino!=canonical.st_ino ||
       before.st_uid!=geteuid() || (before.st_mode&0077) || before.st_nlink!=1 ||
       before.st_size<0 || before.st_size>8192){close(fd);return NULL;}
    char data[8193];size_t used=0;
    while(used<(size_t)before.st_size) {
        ssize_t got=read(fd,data+used,(size_t)before.st_size-used);
        if(got<0 && errno==EINTR)continue;
        if(got<=0)break;
        used+=(size_t)got;
    }
    int coherent=fstat(fd,&after)==0 && before.st_size==after.st_size &&
        before.st_mtim.tv_sec==after.st_mtim.tv_sec && before.st_mtim.tv_nsec==after.st_mtim.tv_nsec &&
        before.st_ctim.tv_sec==after.st_ctim.tv_sec && before.st_ctim.tv_nsec==after.st_ctim.tv_nsec;
    close(fd);
    auth_keyset *set=NULL;
    if(used!=(size_t)before.st_size || !coherent || memchr(data,0,used))goto done;
    data[used]=0;set=calloc(1,sizeof *set);if(!set)goto done;
    char *save=NULL;
    for(char *line=strtok_r(data,"\n",&save);line;line=strtok_r(NULL,"\n",&save)) {
        while(*line==' ' || *line=='\t' || *line=='\r')++line;
        if(!*line || *line=='#')continue;
        char type[16],id[34],secret[66]={0},extra;
        if(sscanf(line,"%15s %33s %65s %c",type,id,secret,&extra)!=3 ||
           set->count==AUTH_CREDENTIAL_LIMIT || !valid_id(id)){OPENSSL_cleanse(secret,sizeof secret);goto invalid;}
        credential *entry=&set->entries[set->count];
        if(!strcmp(type,"token"))entry->policy=AUTH_BEARER;
        else if(!strcmp(type,"hmac"))entry->policy=AUTH_SIGNED;
        else {OPENSSL_cleanse(secret,sizeof secret);goto invalid;}
        if(!unhex(secret,entry->secret,32)){OPENSSL_cleanse(secret,sizeof secret);goto invalid;}
        OPENSSL_cleanse(secret,sizeof secret);
        for(unsigned i=0;i<set->count;++i)if(!strcmp(id,set->entries[i].id))goto invalid;
        strcpy(entry->id,id);++set->count;
    }
    goto done;
invalid:
    auth_keyset_free(set);set=NULL;
done:
    OPENSSL_cleanse(data,sizeof data);return set;
}
void auth_install(auth_keyset *set) {
    auth_keyset_free(active);active=set;
}
int auth_init(void) {
    auth_close();
    if(*config.auth_credentials_file) {
        active=auth_load(config.auth_credentials_file,config.document_root);
        if(!active)return -1;
    }
    bucket_count=2;while(bucket_count<config.auth_nonce_entries*2)bucket_count*=2;
    nonces=calloc(config.auth_nonce_entries,sizeof *nonces);
    buckets=malloc(bucket_count*sizeof *buckets);
    if(!nonces || !buckets || RAND_bytes(hash_seed,sizeof hash_seed)!=1){auth_close();return -1;}
    for(unsigned i=0;i<bucket_count;++i)buckets[i]=NONE;
    return 0;
}
void auth_close(void) {
    auth_keyset_free(active);active=NULL;free(nonces);nonces=NULL;free(buckets);buckets=NULL;
    OPENSSL_cleanse(hash_seed,sizeof hash_seed);
    head=count=bucket_count=0;wall_highwater=mono_highwater=0;memset(&stats,0,sizeof stats);
}
void auth_reload_result(auth_keyset *set) {
    if(set){auth_install(set);++stats.reloads;fprintf(stderr,"Authentication credentials reloaded\n");}
    else {++stats.reload_failures;fprintf(stderr,"Authentication credential reload rejected; previous credentials retained\n");}
}
void auth_snapshot(struct auth_stats *out) {
    *out=stats;out->credentials=active?active->count:0;out->nonce_entries=count;
}
static const credential *find(const char *id,auth_policy policy) {
    if(active)for(unsigned i=0;i<active->count;++i)
        if(active->entries[i].policy==policy && !strcmp(id,active->entries[i].id))return &active->entries[i];
    return NULL;
}
static int reject(int status) { ++stats.rejected;return status; }
static int timestamp(const char *text,int64_t *out) {
    if(!*text || (text[0]=='0' && text[1]))return 0;
    uint64_t value=0;
    for(const unsigned char *p=(const unsigned char *)text;*p;++p) {
        if(*p<'0' || *p>'9' || value>((uint64_t)INT64_MAX-(*p-'0'))/10)return 0;
        value=value*10+(*p-'0');
    }
    *out=(int64_t)value;return 1;
}
static int check_time(int64_t stamp,int64_t wall) {
    if(wall<0 || wall<wall_highwater)return 503;
    wall_highwater=wall;
    int64_t delta=stamp>wall?stamp-wall:wall-stamp;
    if(delta>(int64_t)config.auth_timestamp_window_s){++stats.timestamps_invalid;return 401;}
    return 0;
}
int auth_headers_at(const struct http_request *r,auth_policy policy,int64_t wall) {
    if(policy==AUTH_PUBLIC)return 0;
    if(policy==AUTH_BEARER) {
        const char *p=r->authorization;
        if(!*p)return reject(401);
        if(strncasecmp(p,"Bearer ",7))return reject(401);
        p+=7;const char *dot=strchr(p,'.');
        if(!dot || dot==p || (size_t)(dot-p)>AUTH_ID_LIMIT)return reject(401);
        char id[AUTH_ID_LIMIT+1];memcpy(id,p,(size_t)(dot-p));id[dot-p]=0;
        unsigned char secret[32],actual[32];
        if(!valid_id(id) || !unhex(dot+1,secret,32)){OPENSSL_cleanse(secret,sizeof secret);return reject(401);}
        const credential *entry=find(id,policy);
        int hashed=digest(secret,sizeof secret,actual);
        OPENSSL_cleanse(secret,sizeof secret);
        int valid=hashed && entry && CRYPTO_memcmp(actual,entry->secret,32)==0;
        OPENSSL_cleanse(actual,sizeof actual);
        if(!hashed)return reject(503);
        if(!valid)return reject(401);
        ++stats.accepted;return 0;
    }
    if(!*r->auth_key_id || !*r->auth_timestamp || !*r->auth_nonce || !*r->auth_signature)return reject(401);
    unsigned char nonce[16],signature[32];int64_t stamp;
    if(!valid_id(r->auth_key_id) || !timestamp(r->auth_timestamp,&stamp) ||
       !unhex(r->auth_nonce,nonce,16) || !unhex(r->auth_signature,signature,32))return reject(400);
    if(!find(r->auth_key_id,AUTH_SIGNED))return reject(401);
    int status=check_time(stamp,wall);return status?reject(status):0;
}
int auth_headers(const struct http_request *r,auth_policy policy) {
    return auth_headers_at(r,policy,(int64_t)time(NULL));
}
static unsigned bucket(const unsigned char key[32]) {
    unsigned value=0;memcpy(&value,key,sizeof value);return value&(bucket_count-1);
}
static int reserve(const struct http_request *r,int64_t now,int64_t wall) {
    if(!nonces || !buckets || now<0 || now<mono_highwater ||
       now>INT64_MAX-((int64_t)config.auth_timestamp_window_s*2+1)*1000)return 503;
    mono_highwater=now;
    /* Fixed retention gives insertion order the same order as expiration.
     * A record also survives while its original timestamp is wall-clock valid.
     * Retire at most 32 entries per request to bound reactor cleanup work. */
    for(unsigned retired=0;count && retired<32 && nonces[head].expires<=now && nonces[head].valid_until<wall;++retired) {
        unsigned *link=&buckets[bucket(nonces[head].key)];
        while(*link!=head)link=&nonces[*link].next;
        *link=nonces[head].next;
        head=(head+1)%config.auth_nonce_entries;--count;
    }
    unsigned char input[32+AUTH_ID_LIMIT+1+32],key[32];
    memcpy(input,hash_seed,32);size_t id_length=strlen(r->auth_key_id);
    memcpy(input+32,r->auth_key_id,id_length+1);memcpy(input+33+id_length,r->auth_nonce,32);
    int hashed=digest(input,65+id_length,key);OPENSSL_cleanse(input,sizeof input);
    if(!hashed)return 503;
    unsigned b=bucket(key);
    for(unsigned i=buckets[b];i!=NONE;i=nonces[i].next)
        if(!memcmp(nonces[i].key,key,32)){++stats.replays;return 401;}
    if(count==config.auth_nonce_entries){++stats.capacity_rejections;return 503;}
    unsigned index=(head+count)%config.auth_nonce_entries;
    memcpy(nonces[index].key,key,32);
    int64_t stamp;
    if(!timestamp(r->auth_timestamp,&stamp) || stamp>INT64_MAX-config.auth_timestamp_window_s)return 503;
    nonces[index].valid_until=stamp+config.auth_timestamp_window_s;
    nonces[index].expires=now+((int64_t)config.auth_timestamp_window_s*2+1)*1000;
    nonces[index].next=buckets[b];buckets[b]=index;++count;return 0;
}
int auth_verify_at(const struct http_request *r,const char *body,int64_t wall,int64_t mono_ms) {
    int status=auth_headers_at(r,AUTH_SIGNED,wall);if(status)return status;
    const credential *entry=find(r->auth_key_id,AUTH_SIGNED);
    unsigned char body_digest[32],signature[32],supplied[32];char body_hex[65],canonical[2560];
    if(!digest(body,r->content_length,body_digest))return reject(503);
    hex(body_digest,32,body_hex);
    int length=snprintf(canonical,sizeof canonical,
        "C-HTTP-HMAC-V1\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%zu\n%s\n",
        r->auth_key_id,r->auth_timestamp,r->auth_nonce,r->method,r->target,
        r->content_type,r->content_encoding,r->content_length,body_hex);
    size_t output_length=0;
    if(length<0 || (size_t)length>=sizeof canonical ||
       !EVP_Q_mac(NULL,"HMAC",NULL,"SHA256",NULL,entry->secret,32,
                  (const unsigned char *)canonical,(size_t)length,signature,32,&output_length) || output_length!=32)return reject(503);
    (void)unhex(r->auth_signature,supplied,32);
    if(CRYPTO_memcmp(signature,supplied,32)){++stats.signatures_invalid;return reject(401);}
    status=reserve(r,mono_ms,wall);if(status)return reject(status);
    ++stats.accepted;return 0;
}
int auth_verify(const struct http_request *r,const char *body) {
    struct timespec mono;
    if(clock_gettime(CLOCK_MONOTONIC,&mono)<0)return reject(503);
    return auth_verify_at(r,body,(int64_t)time(NULL),(int64_t)mono.tv_sec*1000+mono.tv_nsec/1000000);
}
