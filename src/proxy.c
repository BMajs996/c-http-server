#define _POSIX_C_SOURCE 200809L
#include "proxy.h"
#include "config.h"
#include "deployment.h"
#include "http_response.h"
#include "http_syntax.h"
#include <arpa/inet.h>
#include <openssl/crypto.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#define WIRE_LIMIT (HEADER_LIMIT+2048)
struct proxy {
    int fd,connecting,head,http11,keep_alive,status,error,committed,finished,runnable,client_retry;
    uint32_t interest;
    int64_t deadline;
    char request[WIRE_LIMIT];size_t request_length,request_offset,body_length,body_offset;
    const char *body;
    char input[32768];size_t input_length,header_scan;
    char output[WIRE_LIMIT];size_t output_length,output_offset;
    char payload[32768];size_t payload_length,payload_offset;
    char frame[32];size_t frame_length,frame_offset,suffix_offset;
    struct http_response response;
    struct http_chunks chunks;
    uint64_t remaining,sent;
    int got_headers,chunk_output,terminal,info_count;size_t info_bytes;
};
static struct proxy_stats stats;
void proxy_snapshot(struct proxy_stats *out){*out=stats;}
static int append(char *buffer,size_t capacity,size_t *used,const char *format,...) {
    va_list args;va_start(args,format);int n=vsnprintf(buffer+*used,capacity-*used,format,args);va_end(args);
    if(n<0 || (size_t)n>=capacity-*used)return -1;
    *used+=(size_t)n;return 0;
}
const char *proxy_allow(void){return "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS";}
int proxy_method(const char *method) {
    size_t length=strlen(method);
    for(const char *p=proxy_allow();*p;) {
        size_t token_length=strcspn(p,",");
        if(length==token_length && !memcmp(method,p,length))return 1;
        p+=token_length;
        if(*p){++p;while(*p==' ')++p;}
    }
    return 0;
}
enum proxy_route_kind proxy_route(const char *target) {
    size_t n=strlen(config.proxy_prefix),path=strcspn(target,"?");
    if(!n)return PROXY_LOCAL;
    if(path>=n && !memcmp(target,config.proxy_prefix,n))return PROXY_FORWARD;
    return path==n-1 && !memcmp(target,config.proxy_prefix,n-1)?PROXY_REDIRECT:PROXY_LOCAL;
}
int proxy_config_validate(void) {
    int enabled=*config.proxy_prefix!=0;
    if(!enabled && !*config.upstream_address && !*config.upstream_host && !config.upstream_port)return 0;
    if(!enabled || !*config.upstream_address || !*config.upstream_host || !config.upstream_port)goto invalid;
    size_t n=strlen(config.proxy_prefix);
    if(n<2 || config.proxy_prefix[0]!='/' || config.proxy_prefix[n-1]!='/' || strstr(config.proxy_prefix,"..") || strstr(config.proxy_prefix,"//"))goto invalid;
    for(size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)config.proxy_prefix[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||strchr("/-_.",c)))goto invalid;
    }
    const char *reserved[]={"/api/","/health/","/ready/","/metrics/"};
    for(size_t i=0;i<4;i++)if(!strncmp(config.proxy_prefix,reserved[i],strlen(reserved[i])))goto invalid;
    struct sockaddr_storage upstream,local;socklen_t ul,ll;
    if(deployment_address(config.upstream_address,config.upstream_port,&upstream,&ul)<0 || deployment_address(config.bind_address,config.port,&local,&ll)<0)goto invalid;
    uint32_t ip=0,own=0;int upstream_v4=0,local_v4=0;
    if(upstream.ss_family==AF_INET){ip=((struct sockaddr_in *)&upstream)->sin_addr.s_addr;upstream_v4=1;}
    else {
        struct in6_addr *address=&((struct sockaddr_in6 *)&upstream)->sin6_addr;
        if(IN6_IS_ADDR_UNSPECIFIED(address) || IN6_IS_ADDR_MULTICAST(address))goto invalid;
        if(IN6_IS_ADDR_V4MAPPED(address)){memcpy(&ip,address->s6_addr+12,sizeof ip);upstream_v4=1;}
    }
    if(local.ss_family==AF_INET){own=((struct sockaddr_in *)&local)->sin_addr.s_addr;local_v4=1;}
    if(upstream_v4) {
        uint32_t host_ip=ntohl(ip);
        if(!host_ip || (host_ip>>28)==14)goto invalid;
        if(local_v4 && config.port==config.upstream_port && (!own || own==ip || ((ntohl(own)>>24)==127 && (host_ip>>24)==127)))goto invalid;
    } else if(local.ss_family==AF_INET6 && config.port==config.upstream_port) {
        struct in6_addr *address=&((struct sockaddr_in6 *)&upstream)->sin6_addr;
        struct in6_addr *listener=&((struct sockaddr_in6 *)&local)->sin6_addr;
        if(IN6_IS_ADDR_UNSPECIFIED(listener) || !memcmp(address,listener,sizeof *address))goto invalid;
    }
    for(const unsigned char *p=(unsigned char *)config.upstream_host;*p;p++)if(*p<=32 || *p>=127)goto invalid;
    char test[512];struct http_request r;
    int len=snprintf(test,sizeof test,"GET / HTTP/1.1\r\nHost: %s\r\n\r\n",config.upstream_host);
    if(len<0 || (size_t)len>=sizeof test || parse_request(test,(size_t)len,&r))goto invalid;
    return 0;
invalid:
    fprintf(stderr,"configuration: invalid/incomplete proxy settings, reserved prefix, or upstream listener loop\n");return -1;
}
proxy *proxy_create(const struct http_request *r,const char *headers,size_t length,int client_fd,int *error) {
    *error=503;proxy *p=calloc(1,sizeof *p);if(!p)return NULL;p->fd=-1;
    *error=400;
    if(http_fields_parse(&p->response.headers,headers,length)<0){*error=431;goto fail;}
    struct http_fields *f=&p->response.headers;
    if(!http_connection_valid(f))goto fail;
    for(size_t i=0;i<f->count;i++)if(!strcasecmp(http_field_name(f,i),"Upgrade")){*error=501;goto fail;}
    p->head=r->head;p->http11=r->http11;p->body_length=r->content_length;
    const char *target=r->target+strlen(config.proxy_prefix)-1;
    if(append(p->request,sizeof p->request,&p->request_length,"%s %s HTTP/1.1\r\nHost: %s\r\n",r->method,target,config.upstream_host)<0)goto large;
    for(size_t i=0;i<f->count;i++) {
        const char *name=http_field_name(f,i),*value=http_field_value(f,i);
        if(http_field_hop(f,i) || !strcasecmp(name,"Host") || !strcasecmp(name,"Content-Length") || !strcasecmp(name,"Forwarded") || !strncasecmp(name,"X-Forwarded-",12))continue;
        if(append(p->request,sizeof p->request,&p->request_length,"%s: %s\r\n",name,value)<0)goto large;
    }
    struct sockaddr_storage peer;socklen_t size=sizeof peer;char address[INET6_ADDRSTRLEN];const void *ip=NULL;
    if(getpeername(client_fd,(struct sockaddr *)&peer,&size)==0) {
        if(peer.ss_family==AF_INET)ip=&((struct sockaddr_in *)&peer)->sin_addr;
        if(peer.ss_family==AF_INET6)ip=&((struct sockaddr_in6 *)&peer)->sin6_addr;
        if(ip && inet_ntop(peer.ss_family,ip,address,sizeof address) && append(p->request,sizeof p->request,&p->request_length,"X-Forwarded-For: %s\r\n",address)<0)goto large;
    }
    if(append(p->request,sizeof p->request,&p->request_length,"X-Forwarded-Proto: %s\r\nVia: 1.1 c-http-server\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",transport_tls_enabled()?"https":"http",p->body_length)<0)goto large;
    *error=0;return p;
large:*error=431;
fail:free(p);return NULL;
}
enum proxy_failure { FAILURE_LOCAL, FAILURE_CONNECT, FAILURE_TIMEOUT, FAILURE_RESPONSE };
static void fail(proxy *p,int code,enum proxy_failure kind) {
    if(p->error)return;
    p->error=code;
    if(kind==FAILURE_CONNECT)stats.connect_failures++;
    if(kind==FAILURE_TIMEOUT)stats.timeouts++;
    if(kind==FAILURE_RESPONSE)stats.invalid_responses++;
}
void proxy_start(proxy *p,const char *body,int keep_alive,int64_t now) {
    p->body=body;p->keep_alive=keep_alive;p->deadline=now+config.upstream_connect_timeout_ms;stats.requests++;
    struct sockaddr_storage address;socklen_t size;
    if(deployment_address(config.upstream_address,config.upstream_port,&address,&size)<0){fail(p,502,FAILURE_CONNECT);return;}
    p->fd=socket(address.ss_family,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(p->fd<0){fail(p,503,FAILURE_LOCAL);return;}
    stats.active++;int flag=1;
    if(setsockopt(p->fd,IPPROTO_TCP,TCP_NODELAY,&flag,sizeof flag)<0){fail(p,502,FAILURE_CONNECT);return;}
    int rc=connect(p->fd,(struct sockaddr *)&address,size);
    if(rc<0 && errno!=EINPROGRESS){fail(p,502,FAILURE_CONNECT);return;}
    p->connecting=rc<0;
    if(!p->connecting)p->deadline=now+config.upstream_response_timeout_ms;
}
static int pending_io(int error){return error==EAGAIN || error==EWOULDBLOCK || error==EINTR;}
static int client_write(proxy *p,transport *client,const char *data,size_t size,
                        size_t *offset,uint32_t *interest,int body) {
    if(*offset==size)return 1;
    /* A TLS write may consume data internally without reporting progress.
     * After attempting final headers, never substitute another response. */
    if(p->got_headers)p->committed=1;
    ssize_t n=transport_write(client,data+*offset,size-*offset,interest);
    p->client_retry=n==-2;
    if(n>0) {
        *offset+=(size_t)n;
        if(body){p->sent+=(uint64_t)n;stats.downstream_bytes+=(uint64_t)n;}
        p->runnable=*offset<size;
        return *offset==size;
    }
    if(n==-2){p->runnable=!*interest;return 0;}
    fail(p,502,FAILURE_LOCAL);return -1;
}
/* Every queued segment shares the same partial-write and TLS retry rules. */
static int flush_client(proxy *p,transport *client,uint32_t *interest) {
    int rc=client_write(p,client,p->output,p->output_length,&p->output_offset,interest,0);
    if(rc<=0 || !p->payload_length)return rc;
    if(p->chunk_output) {
        rc=client_write(p,client,p->frame,p->frame_length,&p->frame_offset,interest,0);
        if(rc<=0)return rc;
    }
    rc=client_write(p,client,p->payload,p->payload_length,&p->payload_offset,interest,1);
    if(rc<=0)return rc;
    if(p->chunk_output) {
        rc=client_write(p,client,"\r\n",2,&p->suffix_offset,interest,0);
        if(rc<=0)return rc;
    }
    p->payload_length=p->payload_offset=p->frame_offset=p->suffix_offset=0;
    return 1;
}
static void consume_input(proxy *p,size_t length) {
    p->header_scan=0;
    p->input_length-=length;
    memmove(p->input,p->input+length,p->input_length);
}
static int response_headers(proxy *p,size_t length) {
    if(http_response_parse(&p->response,p->input,length,p->head)<0)return -1;
    int info=p->response.status<200;
    if(info && (++p->info_count>8 || (p->info_bytes+=length)>HEADER_LIMIT))return -1;
    struct http_fields *f=&p->response.headers;
    p->output_length=p->output_offset=0;
    if(!info)p->got_headers=1;
    p->status=info?0:p->response.status;
    /* Unsolicited 100 is consumed; other informational responses are forwarded. */
    if(!info || (p->http11 && p->response.status!=100)) {
        if(append(p->output,sizeof p->output,&p->output_length,"%s %s\r\n",p->http11?"HTTP/1.1":"HTTP/1.0",f->text+9)<0)return -1;
        for(size_t i=0;i<f->count;i++) {
            const char *name=http_field_name(f,i),*value=http_field_value(f,i);
            if(http_field_hop(f,i) || !strcasecmp(name,"Content-Length"))continue;
            if(append(p->output,sizeof p->output,&p->output_length,"%s: %s\r\n",name,value)<0)return -1;
        }
        if(!info) {
            int unknown_length=p->response.framing==HTTP_CHUNKED || p->response.framing==HTTP_EOF;
            p->chunk_output=unknown_length && p->http11;
            if(p->response.framing==HTTP_LENGTH) {
                if(append(p->output,sizeof p->output,&p->output_length,"Content-Length: %ju\r\n",(uintmax_t)p->response.length)<0)return -1;
            } else if(p->response.framing==HTTP_NONE && (p->head || p->response.status==304)) {
                for(size_t i=0;i<f->count;i++)if(!strcasecmp(http_field_name(f,i),"Content-Length"))
                    if(append(p->output,sizeof p->output,&p->output_length,"Content-Length: %s\r\n",http_field_value(f,i))<0)return -1;
            }
            if(p->chunk_output && append(p->output,sizeof p->output,&p->output_length,"Transfer-Encoding: chunked\r\n")<0)return -1;
            if(!p->http11 && unknown_length)p->keep_alive=0;
            if(append(p->output,sizeof p->output,&p->output_length,"Connection: %s\r\n",p->keep_alive?"keep-alive":"close")<0)return -1;
            p->remaining=p->response.length;
            p->finished=p->response.framing==HTTP_NONE || (p->response.framing==HTTP_LENGTH && !p->remaining);
        }
        if(append(p->output,sizeof p->output,&p->output_length,"Via: 1.1 c-http-server\r\n\r\n")<0)return -1;
    }
    consume_input(p,length);
    return 0;
}
/* Decode only into the fixed payload buffer. Return progress, need input,
 * or invalid framing; chunk lengths never determine allocations. */
static int decode_payload(proxy *p) {
    size_t consumed=p->input_length,produced=sizeof p->payload;int rc=0;
    if(p->response.framing==HTTP_CHUNKED) {
        rc=http_chunks_decode(&p->chunks,p->input,&consumed,p->payload,&produced);
        if(rc<0)return -1;
        if(rc==1)p->finished=1;
    } else {
        if(consumed>produced)consumed=produced;
        if(p->response.framing==HTTP_LENGTH && (uint64_t)consumed>p->remaining)consumed=(size_t)p->remaining;
        produced=consumed;memcpy(p->payload,p->input,produced);
        if(p->response.framing==HTTP_LENGTH){p->remaining-=produced;if(!p->remaining)p->finished=1;}
    }
    if(p->status==205 && produced)return -1;
    consume_input(p,consumed);
    p->payload_length=produced;
    if(produced && p->chunk_output) {int n=snprintf(p->frame,sizeof p->frame,"%zx\r\n",produced);p->frame_length=(size_t)n;}
    return consumed || produced || p->finished;
}
int proxy_step(proxy *p,transport *client,int64_t now,uint32_t *client_interest) {
    *client_interest=0;p->interest=0;p->runnable=0;
    if(p->error)return -1;
    if(now>=p->deadline){fail(p,504,FAILURE_TIMEOUT);return -1;}
    if(p->connecting) {
        struct sockaddr_storage peer;socklen_t size=sizeof peer;
        if(getpeername(p->fd,(struct sockaddr *)&peer,&size)<0) {
            int error=0;socklen_t length=sizeof error;
            if(getsockopt(p->fd,SOL_SOCKET,SO_ERROR,&error,&length)<0 || error){fail(p,502,FAILURE_CONNECT);return -1;}
            p->interest=EPOLLOUT;return 0;
        }
        p->connecting=0;p->deadline=now+config.upstream_response_timeout_ms;
    }
    for(unsigned turn=0;turn<32;turn++) {
        if(p->output_offset<p->output_length || p->payload_length) {
            int rc=flush_client(p,client,client_interest);
            if(rc<=0)return rc;
            continue;
        }
        if(p->got_headers && p->finished) {
            if(p->chunk_output && !p->terminal) {
                memcpy(p->output,"0\r\n\r\n",5);
                p->output_length=5;p->output_offset=0;p->terminal=1;continue;
            }
            return 1;
        }
        if(!p->got_headers) {
            size_t length=http_header_end(p->input,p->input_length,&p->header_scan);
            if(length){if(response_headers(p,length)<0){fail(p,502,FAILURE_RESPONSE);return -1;}continue;}
            if(p->input_length>=HEADER_LIMIT){fail(p,502,FAILURE_RESPONSE);return -1;}
        } else if(p->input_length) {
            int rc=decode_payload(p);
            if(rc<0){fail(p,502,FAILURE_RESPONSE);return -1;}
            if(rc)continue;
        }
        /* Read responses even while request writes are pending (early errors). */
        size_t capacity=p->got_headers?sizeof p->input:HEADER_LIMIT;
        ssize_t n=recv(p->fd,p->input+p->input_length,capacity-p->input_length,0);
        if(n>0){p->input_length+=(size_t)n;stats.upstream_bytes+=(uint64_t)n;continue;}
        if(!n) {
            if(p->got_headers && p->response.framing==HTTP_EOF){p->finished=1;continue;}
            fail(p,502,FAILURE_RESPONSE);return -1;
        }
        if(!pending_io(errno)){fail(p,502,FAILURE_RESPONSE);return -1;}
        p->interest=EPOLLIN;
        if(!p->got_headers) {
            const char *data=NULL;size_t length=0;size_t *offset=NULL;
            if(p->request_offset<p->request_length){data=p->request;length=p->request_length;offset=&p->request_offset;}
            else if(p->body_offset<p->body_length){data=p->body;length=p->body_length;offset=&p->body_offset;}
            if(data) {
                n=send(p->fd,data+*offset,length-*offset,MSG_NOSIGNAL);
                if(n>0){*offset+=(size_t)n;continue;}
                if(n<0 && pending_io(errno))p->interest|=EPOLLOUT;
                else {fail(p,502,FAILURE_RESPONSE);return -1;}
            }
        }
        return 0;
    }
    p->runnable=1;return 0;
}
struct proxy_progress proxy_progress(const proxy *p) {
    if(!p)return (struct proxy_progress){.fd=-1};
    int partial_header=p->output_offset>0 && p->output_offset<p->output_length;
    return (struct proxy_progress){
        .fd=p->fd, .interest=p->interest, .deadline=p->deadline, .sent=p->sent,
        .status=p->committed?p->status:p->error?p->error:p->status,
        .keep_alive=p->keep_alive, .runnable=p->runnable,
        .can_send_error=!p->committed && !p->client_retry && !partial_header
    };
}
void proxy_drain(proxy *p) {
    /* Keep previously prepared wire buffers stable, including pending TLS
     * writes. The owning client still closes at the response boundary. */
    if(p)p->keep_alive=0;
}
void proxy_destroy(proxy *p) {
    if(!p)return;
    if(p->fd>=0){close(p->fd);stats.active--;}
    OPENSSL_cleanse(p,sizeof *p);free(p);
}
