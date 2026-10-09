#define _POSIX_C_SOURCE 200809L
#include "deployment.h"
#include "auth.h"
#include "config.h"
#include "logging.h"
#include "transport.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>

static int ready;
void deployment_set_ready(int value) { ready=value!=0; }
int deployment_ready(void) { return ready; }
int deployment_address(const char *host,unsigned port,struct sockaddr_storage *address,socklen_t *length) {
    memset(address,0,sizeof *address);
    if(!port || port>65535){errno=EINVAL;return -1;}
    struct sockaddr_in ipv4={.sin_family=AF_INET,.sin_port=htons((unsigned short)port)};
    if(inet_pton(AF_INET,host,&ipv4.sin_addr)==1) {
        memcpy(address,&ipv4,sizeof ipv4);*length=sizeof ipv4;return 0;
    }
    struct sockaddr_in6 ipv6={.sin6_family=AF_INET6,.sin6_port=htons((unsigned short)port)};
    if(inet_pton(AF_INET6,host,&ipv6.sin6_addr)==1) {
        memcpy(address,&ipv6,sizeof ipv6);*length=sizeof ipv6;return 0;
    }
    errno=EINVAL;return -1;
}
int deployment_notify(const char *message) {
    const char *path=getenv("NOTIFY_SOCKET");
    if(!path)return 0;
    size_t length=strlen(path);
    struct sockaddr_un address={.sun_family=AF_UNIX};
    if(!length || (path[0]!='/' && path[0]!='@') || (path[0]=='@' && length==1)) {errno=EINVAL;return -1;}
    if(length>=sizeof address.sun_path){errno=ENAMETOOLONG;return -1;}
    memcpy(address.sun_path,path,length+1);
    socklen_t size=(socklen_t)(offsetof(struct sockaddr_un,sun_path)+length+1);
    if(path[0]=='@'){address.sun_path[0]=0;--size;}
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(fd<0)return -1;
    size_t wanted=strlen(message);ssize_t sent;
    do sent=sendto(fd,message,wanted,MSG_NOSIGNAL,(struct sockaddr *)&address,size);while(sent<0 && errno==EINTR);
    int error=sent<0?errno:EIO;close(fd);
    if(sent!=(ssize_t)wanted){errno=error;return -1;}
    return 0;
}
int deployment_check_config(void) {
    struct sockaddr_storage address;socklen_t length;
    if(deployment_address(config.bind_address,config.port,&address,&length)<0) {
        fprintf(stderr,"configuration: bind_address must be a numeric IPv4 or IPv6 address\n");return -1;
    }
    int root=open(config.document_root,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(root<0){perror("document root");return -1;}
    close(root);
    if(transport_init()<0)return -1;
    transport_close();
    if(*config.auth_credentials_file) {
        auth_keyset *set=auth_load(config.auth_credentials_file,config.document_root);
        if(!set){fprintf(stderr,"Authentication credential validation failed\n");return -1;}
        auth_keyset_free(set);
    }
    if(logging_check()<0)return -1;
    return 0;
}
