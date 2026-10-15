#include "http_response.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void check(const char *wire,int head,int valid,enum http_framing framing) {
    struct http_response r;int rc=http_response_parse(&r,wire,strlen(wire),head);
    assert((rc==0)==valid);if(valid)assert(r.framing==framing);
}
static void chunks(const char *wire,const char *expected,int valid) {
    struct http_chunks c={0};char decoded[128];size_t used=0,offset=0;int rc=0;
    while(offset<strlen(wire) && rc==0) {
        size_t in=1,out=1;rc=http_chunks_decode(&c,wire+offset,&in,decoded+used,&out);
        assert(in<=1 && out<=1);offset+=in;used+=out;assert(in || out || rc);
    }
    assert(valid?rc==1:rc!=1);
    if(valid)assert(used==strlen(expected) && !memcmp(decoded,expected,used));
}
int main(void) {
    const char *persistence[]={"HTTP/1.1 200 OK\r\nConnection: keep-alive, X-Test\r\n\r\n","HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nConnection: ClOsE\r\n\r\n","HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n\r\n"};
    for(size_t i=0;i<sizeof persistence/sizeof persistence[0];i++) {
        struct http_response r;assert(!http_response_parse(&r,persistence[i],strlen(persistence[i]),0));
        assert(r.persistent==(i==0));
    }
    check("HTTP/1.1 200 OK\r\nContent-Length: 42\r\n\r\n",0,1,HTTP_LENGTH);
    check("HTTP/1.0 200 OK\r\n\r\n",0,1,HTTP_EOF);
    check("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",0,1,HTTP_CHUNKED);
    check("HTTP/1.1 200 OK\r\nContent-Length: 42\r\n\r\n",1,1,HTTP_NONE);
    check("HTTP/1.1 204 No Content\r\n\r\n",0,1,HTTP_NONE);
    check("HTTP/1.1 304 Not Modified\r\nContent-Length: 42\r\n\r\n",0,1,HTTP_NONE);
    const char *bad[]={"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n","HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n","HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\n","HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n","HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n","HTTP/1.1 200 OK\r\nConnection: Content-Length\r\n\r\n","HTTP/1.1 101 Switching Protocols\r\n\r\n","HTTP/1.1 200 OK\r\n folded: x\r\n\r\n"};
    for(size_t i=0;i<sizeof bad/sizeof bad[0];i++)check(bad[i],0,0,HTTP_NONE);
    chunks("3;foo=\"a;b\"\r\nabc\r\n2\r\nde\r\n0\r\nX-End: yes\r\n\r\n","abcde",1);
    chunks("0\r\n\r\n","",1);
    chunks("1\r\nxBAD","",0);
    chunks("10000000000000000\r\n","",0);
    chunks("1;foo=\"unclosed\r\nx\r\n0\r\n\r\n","",0);
    chunks("0\r\nContent-Length: 3\r\n\r\n","",0);
    chunks("0\r\n\n","",0);
    puts("Response framing and incremental chunk decoder checks passed");return 0;
}
