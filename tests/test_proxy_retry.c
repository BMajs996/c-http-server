#define _POSIX_C_SOURCE 200809L
#include "proxy.h"
#include "config.h"
#include <assert.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

/* Model the transport contract: retry needs identical pointer/data/length,
 * even when no application bytes have been reported as written. */
struct transport {
    int blocked, partial;
    const void *pending;
    size_t pending_length, used;
    char original[16384], written[16384];
};
int transport_init(void){return 0;}
void transport_close(void){}
int transport_tls_enabled(void){return 1;}
uint64_t transport_handshakes(void){return 0;}
uint64_t transport_failed_handshakes(void){return 0;}
ssize_t transport_write(transport *t,const void *data,size_t length,uint32_t *interest) {
    assert(length<sizeof t->original);
    if(t->blocked) {
        t->blocked=0;t->pending=data;t->pending_length=length;
        memcpy(t->original,data,length);*interest=EPOLLOUT;return -2;
    }
    if(t->pending) {
        assert(t->pending==data && t->pending_length==length);
        assert(!memcmp(t->original,data,length));t->pending=NULL;
    }
    if(t->partial){t->partial=0;if(length>4)length=4;}
    assert(t->used+length<sizeof t->written);
    memcpy(t->written+t->used,data,length);t->used+=length;
    return (ssize_t)length;
}
static proxy *transaction(const char *response,int *backend) {
    config_defaults();strcpy(config.proxy_prefix,"/proxy/");
    strcpy(config.upstream_address,"127.0.0.1");strcpy(config.upstream_host,"a");
    int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(listener,(struct sockaddr *)&address,sizeof address));assert(!listen(listener,1));
    socklen_t size=sizeof address;assert(!getsockname(listener,(struct sockaddr *)&address,&size));
    config.upstream_port=ntohs(address.sin_port);
    struct http_request request={.http11=1};strcpy(request.method,"GET");strcpy(request.target,"/proxy/x");
    const char *headers="GET /proxy/x HTTP/1.1\r\nHost: a\r\n\r\n";int error;
    proxy *p=proxy_create(&request,headers,strlen(headers),-1,&error);assert(p && !error);
    proxy_start(p,"",1,0);*backend=accept(listener,NULL,NULL);assert(*backend>=0);close(listener);
    assert(send(*backend,response,strlen(response),0)==(ssize_t)strlen(response));
    return p;
}
static void block_write(proxy *p,transport *client) {
    for(int turn=1;turn<100;turn++) {
        uint32_t interest;assert(proxy_step(p,client,turn,&interest)==0);
        if(client->pending || client->used)return;
    }
    assert(!"Client write was not reached");
}
int main(void) {
    const char *final="HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    int backend;proxy *p=transaction(final,&backend);transport client={.blocked=1};
    block_write(p,&client);assert(!proxy_progress(p).can_send_error);
    proxy_drain(p);int rc=0;
    for(int turn=100;turn<200 && !rc;turn++){uint32_t interest;rc=proxy_step(p,&client,turn,&interest);}
    assert(rc==1 && proxy_progress(p).sent==2 && !proxy_progress(p).keep_alive);
    assert(strstr(client.written,"Connection: keep-alive\r\n"));
    assert(!memcmp(client.written+client.used-2,"ok",2));
    proxy_destroy(p);close(backend);

    p=transaction(final,&backend);client=(transport){.blocked=1};block_write(p,&client);
    uint32_t interest;assert(proxy_step(p,&client,proxy_progress(p).deadline,&interest)==-1);
    assert(!proxy_progress(p).can_send_error && proxy_progress(p).status==200);
    proxy_destroy(p);close(backend);

    const char *info="HTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\n";
    for(int partial=0;partial<2;partial++) {
        p=transaction(info,&backend);client=(transport){.blocked=!partial,.partial=partial};
        block_write(p,&client);
        assert(proxy_step(p,&client,proxy_progress(p).deadline,&interest)==-1);
        assert(!proxy_progress(p).can_send_error && proxy_progress(p).status==504);
        proxy_destroy(p);close(backend);
    }
    struct proxy_stats stats;proxy_snapshot(&stats);assert(!stats.active);
    puts("Pending/partial client writes remain stable during drain and timeout");return 0;
}
