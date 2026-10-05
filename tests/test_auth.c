#define _POSIX_C_SOURCE 200809L
#include "auth.h"
#include "config.h"
#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static const char *key_hex="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
static void sign_request(struct http_request *r,int64_t stamp,unsigned nonce) {
    snprintf(r->auth_timestamp,sizeof r->auth_timestamp,"%lld",(long long)stamp);
    snprintf(r->auth_nonce,sizeof r->auth_nonce,"%032x",nonce);
    unsigned char key[32],digest[32],signature[32];size_t length=0;
    for(unsigned i=0;i<32;++i)key[i]=(unsigned char)i;
    assert(EVP_Q_digest(NULL,"SHA256",NULL,"{}",2,digest,&length)==1);
    char hash[65],canonical[512];
    for(unsigned i=0;i<32;++i)sprintf(hash+2*i,"%02x",digest[i]);
    int n=snprintf(canonical,sizeof canonical,"C-HTTP-HMAC-V1\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%zu\n%s\n",
        r->auth_key_id,r->auth_timestamp,r->auth_nonce,r->method,r->target,r->content_type,r->content_encoding,r->content_length,hash);
    assert(n>0 && n<(int)sizeof canonical);
    assert(EVP_Q_mac(NULL,"HMAC",NULL,"SHA256",NULL,key,32,(unsigned char *)canonical,(size_t)n,signature,32,&length));
    for(unsigned i=0;i<32;++i)sprintf(r->auth_signature+2*i,"%02x",signature[i]);
}
int main(void) {
    char directory[]="/tmp/c-http-auth-XXXXXX";assert(mkdtemp(directory));
    char root[256],path[256];snprintf(root,sizeof root,"%s/public",directory);assert(mkdir(root,0700)==0);
    snprintf(path,sizeof path,"%s/keys.credentials",directory);
    FILE *file=fopen(path,"w");assert(file);
    fprintf(file,"token token-a 630dcd2966c4336691125448bbb25b4ff412a49c732db2c8abc1b8581bd710dd\nhmac sign-a %s\n",key_hex);
    assert(fclose(file)==0 && chmod(path,0600)==0);
    config_defaults();strcpy(config.document_root,root);strcpy(config.auth_credentials_file,path);config.auth_nonce_entries=2;
    assert(auth_init()==0);
    struct http_request r={0};strcpy(r.authorization,"Bearer token-a.");strcat(r.authorization,key_hex);
    assert(auth_headers_at(&r,AUTH_BEARER,1000)==0);
    r.authorization[20]='f';assert(auth_headers_at(&r,AUTH_BEARER,1000)==401);
    memset(&r,0,sizeof r);strcpy(r.auth_key_id,"sign-a");strcpy(r.method,"POST");
    strcpy(r.target,"/api/private/echo?x=1");strcpy(r.content_type,"application/json");r.content_length=2;
    sign_request(&r,1000,0);
    /* Fixture generated independently with Python hashlib/hmac. */
    assert(!strcmp(r.auth_signature,"66cf805d1ffa3b0e0b667a201c06c636f8ea4642291d5d72c9da862afc4172bf"));
    strcpy(r.method,"GET");assert(auth_verify_at(&r,"{}",1000,100000)==401);strcpy(r.method,"POST");
    assert(auth_verify_at(&r,"{}",1000,100000)==0);
    assert(auth_verify_at(&r,"{}",1000,100000)==401);
    auth_install(auth_load(path,root));assert(auth_verify_at(&r,"{}",1000,100001)==401);
    sign_request(&r,1060,1);assert(auth_verify_at(&r,"{}",1000,100002)==0);
    sign_request(&r,1000,2);assert(auth_verify_at(&r,"[]",1000,100003)==401);
    assert(auth_verify_at(&r,"{}",1000,100003)==503);
    sign_request(&r,1060,1);assert(auth_verify_at(&r,"{}",1120,220000)==401);
    sign_request(&r,1121,2);assert(auth_verify_at(&r,"{}",1121,221003)==0);
    sign_request(&r,1121,3);assert(auth_verify_at(&r,"{}",1120,221004)==503); /* Wall clock rollback. */
    assert(auth_verify_at(&r,"{}",1121,221002)==503); /* Monotonic rollback. */
    assert(auth_init()==0);
    sign_request(&r,940,4);assert(auth_headers_at(&r,AUTH_SIGNED,1000)==0);
    sign_request(&r,939,4);assert(auth_headers_at(&r,AUTH_SIGNED,1000)==401);
    sign_request(&r,1060,4);assert(auth_headers_at(&r,AUTH_SIGNED,1000)==0);
    sign_request(&r,1061,4);assert(auth_headers_at(&r,AUTH_SIGNED,1000)==401);
    assert(auth_init()==0);
    sign_request(&r,1060,5);assert(auth_verify_at(&r,"{}",1000,100000)==0);
    /* Even an unobserved clock slowdown cannot expire a still-valid signature. */
    assert(auth_verify_at(&r,"{}",1120,222000)==401);
    strcpy(r.auth_timestamp,"01000");assert(auth_headers_at(&r,AUTH_SIGNED,1000)==400);
    strcpy(r.auth_timestamp,"9223372036854775808");assert(auth_headers_at(&r,AUTH_SIGNED,1000)==400);
    auth_close();assert(unlink(path)==0 && rmdir(root)==0 && rmdir(directory)==0);
    puts("Token, signature fixture, timestamp boundaries, replay retention, capacity, rotation, and clock rollback checks passed.");
    return 0;
}
