#define _POSIX_C_SOURCE 200809L
#include "static_files.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode once; reject ambiguous/control characters and dot components.
 * Hidden paths are deliberately unavailable, including .git and .env.
 */
static int decode_path(const char *target, char *path, size_t size) {
    size_t used = 0;
    if (*target++ != '/') return 400;
    while (*target && *target != '?') {
        unsigned char c = (unsigned char)*target++;
        if (c == '%') {
            if (!target[0] || !target[1]) return 400;
            int hi = hex_digit((unsigned char)target[0]);
            int lo = hex_digit((unsigned char)target[1]);
            if (hi < 0 || lo < 0) return 400;
            c = (unsigned char)(hi * 16 + lo);
            target += 2;
        }
        if (c < 32 || c == 127 || c == '\\' || c == '#' || c == '%') return 400;
        if (used + 1 >= size) return 414;
        path[used++] = (char)c;
    }
    path[used] = '\0';
    return 0;
}

static const char *mime_type(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) return "application/octet-stream";
    struct { const char *ext; const char *type; } types[] = {
        {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
        {".json", "application/json"}, {".txt", "text/plain; charset=utf-8"},
        {".svg", "image/svg+xml"}, {".png", "image/png"}, {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".ico", "image/x-icon"},
        {".webp", "image/webp"}, {".pdf", "application/pdf"}, {".woff2", "font/woff2"}
    };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; ++i)
        if (strcmp(ext, types[i].ext) == 0) return types[i].type;
    return "application/octet-stream";
}

static int error_status(int error) {
    if (error == ENOENT) return 404;
    if (error == ELOOP || error == EACCES || error == EPERM || error == ENOTDIR) return 403;
    if (error == ENAMETOOLONG) return 414;
    return 500;
}

int prepare_static(int root_fd,const char *target,int gzip_q,int identity_q,struct static_file *file) {
    char path[2048];
    int status=decode_path(target,path,sizeof path);
    if(status)return status;
    int current=dup(root_fd),parent=-1;
    if(current<0)return 500;
    char *component=path;
    const char *filename="index.html";
    while(*component) {
        char *slash=strchr(component,'/');
        if(slash)*slash=0;
        if(*component=='.' || !*component){status=403;goto failed;}
        int flags=O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK;
        if(slash)flags|=O_DIRECTORY;
        int next=openat(current,component,flags);
        if(next<0){status=error_status(errno);goto failed;}
        if(slash)close(current);
        else parent=current;
        current=next;filename=component;
        if(!slash)break;
        component=slash+1;
    }
    struct stat source,metadata;
    if(fstat(current,&source)<0){status=500;goto failed;}
    if(S_ISDIR(source.st_mode)) {
        if(parent>=0)close(parent);
        parent=current;
        current=openat(parent,"index.html",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(current<0){status=error_status(errno);goto failed;}
        filename="index.html";
        if(fstat(current,&source)<0){status=500;goto failed;}
    }
    if(!S_ISREG(source.st_mode) || source.st_size<0){status=403;goto failed;}
    metadata=source;file->gzip=0;
    if(gzip_q>0 && gzip_q>=identity_q) {
        char sidecar[2052];
        int n=snprintf(sidecar,sizeof sidecar,"%s.gz",filename);
        int compressed=n>0 && (size_t)n<sizeof sidecar?
            openat(parent,sidecar,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK):-1;
        if(compressed>=0) {
            struct stat stamp;
            if(fstat(compressed,&stamp)<0){close(compressed);status=500;goto failed;}
            unsigned char magic[2];
            int fresh=stamp.st_mtim.tv_sec>source.st_mtim.tv_sec ||
                (stamp.st_mtim.tv_sec==source.st_mtim.tv_sec && stamp.st_mtim.tv_nsec>=source.st_mtim.tv_nsec);
            ssize_t count=0;
            if(S_ISREG(stamp.st_mode) && stamp.st_size>=2 && fresh) {
                do count=pread(compressed,magic,2,0); while(count<0 && errno==EINTR);
                if(count<0){close(compressed);status=500;goto failed;}
            }
            if(count==2 && magic[0]==0x1f && magic[1]==0x8b) {
                close(current);current=compressed;metadata=stamp;file->gzip=1;
            } else close(compressed);
        } else if(n>0 && (size_t)n<sizeof sidecar && errno!=ENOENT && errno!=ENOTDIR &&
                  errno!=ELOOP && errno!=EACCES && errno!=EPERM && errno!=ENAMETOOLONG) {
            status=error_status(errno);goto failed;
        }
    }
    if(!file->gzip && !identity_q){status=406;goto failed;}
    int n=snprintf(file->etag,sizeof file->etag,
        "W/\"%s-%jx-%jx-%jx-%jx-%jx-%jx-%jx-%jx-%jx-%jx\"",
        file->gzip?"gzip":"identity",(uintmax_t)metadata.st_dev,(uintmax_t)metadata.st_ino,
        (uintmax_t)metadata.st_size,(uintmax_t)metadata.st_mtim.tv_sec,(uintmax_t)metadata.st_mtim.tv_nsec,
        (uintmax_t)source.st_dev,(uintmax_t)source.st_ino,(uintmax_t)source.st_size,
        (uintmax_t)source.st_mtim.tv_sec,(uintmax_t)source.st_mtim.tv_nsec);
    struct tm utc;
    if(n<0 || (size_t)n>=sizeof file->etag || !gmtime_r(&metadata.st_mtime,&utc) ||
       !strftime(file->last_modified,sizeof file->last_modified,"%a, %d %b %Y %H:%M:%S GMT",&utc)) {
        status=500;goto failed;
    }
    close(parent);
    file->data=NULL;file->stamp=metadata;file->fd=current;
    file->length=metadata.st_size;file->type=mime_type(filename);
    return 0;
failed:
    if(current>=0)close(current);
    if(parent>=0)close(parent);
    return status;
}
