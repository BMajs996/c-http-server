#include "transport.h"
#include "config.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#ifdef WITH_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
static SSL_CTX *context;
#endif
struct transport {
    int fd, handshake_done;
#ifdef WITH_TLS
    SSL *ssl;
#endif
};
static uint64_t completed, failed;
#ifdef WITH_TLS
static int alpn(SSL *ssl, const unsigned char **out, unsigned char *out_length,
                const unsigned char *input, unsigned int length, void *arg) {
    (void)ssl;(void)arg;
    unsigned int pos=0;
    while(pos<length) {
        unsigned int size=input[pos++];
        if(!size || size>length-pos)return SSL_TLSEXT_ERR_ALERT_FATAL;
        if(size==8 && !memcmp(input+pos,"http/1.1",8)) {
            *out=(const unsigned char *)"http/1.1";*out_length=8;return SSL_TLSEXT_ERR_OK;
        }
        pos+=size;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}
static int ssl_result(transport *t,int rc,uint32_t *interest) {
    int error=SSL_get_error(t->ssl,rc);
    if(error==SSL_ERROR_WANT_READ){*interest=EPOLLIN;return 0;}
    if(error==SSL_ERROR_WANT_WRITE){*interest=EPOLLOUT;return 0;}
    if(error==SSL_ERROR_ZERO_RETURN)return -2;
    return -1;
}
#endif
int transport_init(void) {
    if(!*config.tls_cert)return 0;
#ifdef WITH_TLS
    context=SSL_CTX_new(TLS_server_method());
    if(!context || !SSL_CTX_set_min_proto_version(context,TLS1_2_VERSION) ||
       SSL_CTX_use_certificate_chain_file(context,config.tls_cert)!=1 ||
       SSL_CTX_use_PrivateKey_file(context,config.tls_key,SSL_FILETYPE_PEM)!=1 ||
       SSL_CTX_check_private_key(context)!=1) {
        fprintf(stderr,"TLS certificate/key initialization failed\n");ERR_print_errors_fp(stderr);
        transport_close();return -1;
    }
    SSL_CTX_set_options(context,SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(context,SSL_MODE_ENABLE_PARTIAL_WRITE);
    SSL_CTX_set_alpn_select_cb(context,alpn,NULL);
    return 0;
#else
    fprintf(stderr,"TLS requested but this build has TLS=0\n");return -1;
#endif
}
void transport_close(void) {
#ifdef WITH_TLS
    SSL_CTX_free(context);context=NULL;
#endif
}
int transport_tls_enabled(void) {
#ifdef WITH_TLS
    return context!=NULL;
#else
    return 0;
#endif
}
transport *transport_create(int fd) {
    transport *t=calloc(1,sizeof *t);if(!t)return NULL;t->fd=fd;
#ifdef WITH_TLS
    if(context) {
        t->ssl=SSL_new(context);
        if(!t->ssl || SSL_set_fd(t->ssl,fd)!=1){SSL_free(t->ssl);free(t);return NULL;}
        SSL_set_accept_state(t->ssl);
    }
#endif
    return t;
}
void transport_destroy(transport *t) {
    if(!t)return;
#ifdef WITH_TLS
    if(t->ssl && !t->handshake_done)++failed;
    SSL_free(t->ssl);
#endif
    free(t);
}
int transport_handshake(transport *t,uint32_t *interest) {
    (void)t;(void)interest;
#ifdef WITH_TLS
    if(t->ssl) {
        ERR_clear_error();int rc=SSL_accept(t->ssl);
        if(rc!=1){int result=ssl_result(t,rc,interest);return result==0?0:-1;}
        t->handshake_done=1;++completed;
    }
#endif
    return 1;
}
ssize_t transport_read(transport *t,void *data,size_t length,uint32_t *interest) {
#ifdef WITH_TLS
    if(t->ssl) {
        size_t count;ERR_clear_error();int rc=SSL_read_ex(t->ssl,data,length,&count);
        if(rc==1)return (ssize_t)count;
        int result=ssl_result(t,rc,interest);
        return result==0?-2:result==-2?0:-1;
    }
#endif
    ssize_t n=recv(t->fd,data,length,0);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){*interest=EPOLLIN;return -2;}
    if(n<0 && errno==EINTR)return -2;
    return n;
}
ssize_t transport_write(transport *t,const void *data,size_t length,uint32_t *interest) {
#ifdef WITH_TLS
    if(t->ssl) {
        size_t count;ERR_clear_error();int rc=SSL_write_ex(t->ssl,data,length,&count);
        if(rc==1)return (ssize_t)count;
        return ssl_result(t,rc,interest)==0?-2:-1;
    }
#endif
    ssize_t n=send(t->fd,data,length,0);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){*interest=EPOLLOUT;return -2;}
    if(n<0 && errno==EINTR)return -2;
    return n;
}
ssize_t transport_sendfile(transport *t,int file_fd,off_t *offset,size_t length,
                           uint32_t *interest) {
#ifdef WITH_TLS
    if(t->ssl)return -3;
#endif
    ssize_t n=sendfile(t->fd,file_fd,offset,length);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {
        *interest=EPOLLOUT;return -2;
    }
    if(n<0 && errno==EINTR)return -2;
    if(n<0 && (errno==EINVAL || errno==ENOSYS || errno==EOPNOTSUPP ||
               errno==ESPIPE || errno==EOVERFLOW))return -3;
    return n;
}
int transport_shutdown(transport *t,uint32_t *interest) {
    (void)t;(void)interest;
#ifdef WITH_TLS
    if(t->ssl) {
        ERR_clear_error();int rc=SSL_shutdown(t->ssl);
        /* One-way close_notify: do not wait for peer shutdown after our alert. */
        if(rc>=0)return 1;
        return ssl_result(t,rc,interest)==0?0:-1;
    }
#endif
    return 1;
}
uint64_t transport_handshakes(void){return completed;}
uint64_t transport_failed_handshakes(void){return failed;}
