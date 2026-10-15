#define _POSIX_C_SOURCE 200809L
#include "upstream.h"
#include "deployment.h"
#include "config.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef WITH_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
static SSL_CTX *context;
#endif
struct upstream {
    int fd,connecting,ready,fatal,closing;
    int64_t connected_at,born;
    unsigned requests;
    uint32_t interest;
    enum upstream_operation pending;
#ifdef WITH_TLS
    SSL *ssl;
    const void *write_buffer;size_t write_size;
    unsigned char probe;
#endif
};
static struct upstream_tls_stats stats;
void upstream_tls_snapshot(struct upstream_tls_stats *out){*out=stats;}
#ifdef WITH_TLS
static int no_password(char *buffer,int size,int writing,void *arg) {
    (void)buffer;(void)size;(void)writing;(void)arg;return 0;
}
static int tls_result(upstream *u,int rc,uint32_t *interest) {
    int error=SSL_get_error(u->ssl,rc);
    if(error==SSL_ERROR_WANT_READ || error==SSL_ERROR_WANT_WRITE) {
        *interest=error==SSL_ERROR_WANT_READ?EPOLLIN:EPOLLOUT;u->interest=*interest;return -2;
    }
    u->fatal=1;
    return error==SSL_ERROR_ZERO_RETURN?0:-1;
}
#endif
int upstream_init(void) {
    if(!config.upstream_tls)return 0;
#ifdef WITH_TLS
    context=SSL_CTX_new(TLS_client_method());
    if(!context)goto fail;
    SSL_CTX_set_default_passwd_cb(context,no_password);
    SSL_CTX_set_verify(context,SSL_VERIFY_PEER,NULL);
    SSL_CTX_set_options(context,SSL_OP_NO_COMPRESSION|SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_mode(context,SSL_MODE_ENABLE_PARTIAL_WRITE);
    if(!SSL_CTX_set_min_proto_version(context,TLS1_2_VERSION) ||
       !SSL_CTX_load_verify_locations(context,config.upstream_tls_ca_file,NULL) ||
       SSL_CTX_set_alpn_protos(context,(const unsigned char *)"\x08http/1.1",9)!=0)goto fail;
    if(*config.upstream_tls_client_cert &&
       (SSL_CTX_use_certificate_chain_file(context,config.upstream_tls_client_cert)!=1 ||
        SSL_CTX_use_PrivateKey_file(context,config.upstream_tls_client_key,SSL_FILETYPE_PEM)!=1 ||
        SSL_CTX_check_private_key(context)!=1))goto fail;
    return 0;
fail:
    fprintf(stderr,"upstream TLS: cannot load CA or matching client certificate/key\n");
    upstream_close();return -1;
#else
    fprintf(stderr,"upstream TLS requires a TLS=1 build\n");return -1;
#endif
}
void upstream_close(void) {
#ifdef WITH_TLS
    SSL_CTX_free(context);context=NULL;
#endif
}
upstream *upstream_open(int64_t now,int *error) {
    struct sockaddr_storage address;socklen_t size;
    *error=502;
    if(deployment_address(config.upstream_address,config.upstream_port,&address,&size)<0)return NULL;
    *error=503;upstream *u=calloc(1,sizeof *u);if(!u)return NULL;
    u->fd=socket(address.ss_family,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(u->fd<0){free(u);return NULL;}
    int flag=1;
    if(setsockopt(u->fd,IPPROTO_TCP,TCP_NODELAY,&flag,sizeof flag)<0)goto fail;
#ifdef WITH_TLS
    if(config.upstream_tls) {
        unsigned char ip[16];const char *name=config.upstream_tls_server_name;
        u->ssl=context?SSL_new(context):NULL;
        if(!u->ssl || SSL_set_fd(u->ssl,u->fd)!=1)goto fail;
        int numeric=inet_pton(AF_INET,name,ip)==1 || inet_pton(AF_INET6,name,ip)==1;
        if(numeric) {
            if(X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(u->ssl),name)!=1)goto fail;
        } else {
            SSL_set_hostflags(u->ssl,X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS|X509_CHECK_FLAG_NEVER_CHECK_SUBJECT);
            if(SSL_set1_host(u->ssl,name)!=1 || SSL_set_tlsext_host_name(u->ssl,name)!=1)goto fail;
        }
        SSL_set_connect_state(u->ssl);
    }
#endif
    *error=502;int rc=connect(u->fd,(struct sockaddr *)&address,size);
    if(rc<0 && errno!=EINPROGRESS)goto fail;
    u->born=now;u->connecting=rc<0;u->connected_at=u->connecting?0:now;*error=0;return u;
fail:upstream_destroy(u);return NULL;
}
int upstream_ready(upstream *u,int64_t now,uint32_t *interest) {
    if(u->ready)return 1;
    if(u->connecting) {
        struct sockaddr_storage peer;socklen_t size=sizeof peer;
        if(getpeername(u->fd,(struct sockaddr *)&peer,&size)<0) {
            int error=0;socklen_t length=sizeof error;
            if(getsockopt(u->fd,SOL_SOCKET,SO_ERROR,&error,&length)<0 || error)return -1;
            *interest=EPOLLOUT;return 0;
        }
        u->connecting=0;u->connected_at=now;
    }
#ifdef WITH_TLS
    if(u->ssl) {
        ERR_clear_error();int rc=SSL_connect(u->ssl);
        if(rc!=1) {
            int result=tls_result(u,rc,interest);
            if(result==-2)return 0;
            stats.handshake_failures++;
            if(SSL_get_verify_result(u->ssl)!=X509_V_OK)stats.verification_failures++;
            return -1;
        }
        const unsigned char *protocol;unsigned length;
        SSL_get0_alpn_selected(u->ssl,&protocol,&length);
        if(SSL_get_verify_result(u->ssl)!=X509_V_OK || (length && (length!=8 || memcmp(protocol,"http/1.1",8)))) {
            stats.handshake_failures++;u->fatal=1;return -1;
        }
        stats.handshakes++;
    }
#endif
    u->ready=1;u->interest=0;return 1;
}
static ssize_t result(ssize_t n,uint32_t *interest,uint32_t wanted) {
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)){*interest=wanted;return -2;}
    return n;
}
ssize_t upstream_read(upstream *u,void *data,size_t size,uint32_t *interest) {
#ifdef WITH_TLS
    if(u->ssl) {
        if(u->pending==UPSTREAM_WRITE){*interest=u->interest;return -2;}
        size_t bytes;ERR_clear_error();int rc=SSL_read_ex(u->ssl,data,size,&bytes);
        if(rc==1){u->pending=UPSTREAM_NONE;u->interest=0;return (ssize_t)bytes;}
        int ret=tls_result(u,rc,interest);
        u->pending=ret==-2 && *interest==EPOLLOUT?UPSTREAM_READ:UPSTREAM_NONE;
        if(ret==-1)stats.io_failures++;
        return ret;
    }
#endif
    return result(recv(u->fd,data,size,0),interest,EPOLLIN);
}
ssize_t upstream_write(upstream *u,const void *data,size_t size,uint32_t *interest) {
#ifdef WITH_TLS
    if(u->ssl) {
        if(u->pending==UPSTREAM_READ){*interest=u->interest;return -2;}
        if(u->pending==UPSTREAM_WRITE && (data!=u->write_buffer || size!=u->write_size)){u->fatal=1;return -1;}
        size_t bytes;ERR_clear_error();int rc=SSL_write_ex(u->ssl,data,size,&bytes);
        if(rc==1){u->pending=UPSTREAM_NONE;u->interest=0;return (ssize_t)bytes;}
        int ret=tls_result(u,rc,interest);
        u->pending=ret==-2?UPSTREAM_WRITE:UPSTREAM_NONE;u->write_buffer=data;u->write_size=size;
        if(ret==-1)stats.io_failures++;
        return ret;
    }
#endif
    return result(send(u->fd,data,size,MSG_NOSIGNAL),interest,EPOLLOUT);
}
struct upstream_state upstream_state(const upstream *u) {
    return u?(struct upstream_state){u->fd,u->pending,u->connected_at,u->born,u->requests}:(struct upstream_state){.fd=-1};
}
void upstream_begin(upstream *u,int64_t now){u->requests++;if(u->ready)u->connected_at=now;}
int upstream_clean(upstream *u,uint32_t *interest) {
    *interest=EPOLLIN;
    if(!u->ready || u->fatal || u->pending!=UPSTREAM_NONE)return 0;
#ifdef WITH_TLS
    if(u->ssl) {
        if(SSL_pending(u->ssl)>0){u->fatal=1;return 0;}
        size_t bytes;ERR_clear_error();int rc=SSL_peek_ex(u->ssl,&u->probe,1,&bytes);
        if(rc==1){u->fatal=1;return 0;}
        return tls_result(u,rc,interest)==-2;
    }
#endif
    char byte;ssize_t n=recv(u->fd,&byte,1,MSG_PEEK|MSG_DONTWAIT);
    return n<0 && (errno==EAGAIN || errno==EWOULDBLOCK);
}
int upstream_shutdown(upstream *u,uint32_t *interest) {
#ifdef WITH_TLS
    if(u->ssl && u->ready && !u->fatal && u->pending==UPSTREAM_NONE && (u->closing || u->interest!=EPOLLOUT)) {
        u->closing=1;ERR_clear_error();int rc=SSL_shutdown(u->ssl);
        if(rc==1)return 1;
        if(rc==0){*interest=EPOLLIN;u->interest=EPOLLIN;return 0;}
        return tls_result(u,rc,interest)==-2?0:-1;
    }
#endif
    (void)u;(void)interest;return 1;
}
void upstream_destroy(upstream *u) {
    if(!u)return;
#ifdef WITH_TLS
    SSL_free(u->ssl);
#endif
    close(u->fd);free(u);
}
