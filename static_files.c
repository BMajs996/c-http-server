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

int prepare_static(int root_fd, const char *target, struct static_file *file) {
    char path[2048];
    int status = decode_path(target, path, sizeof path);
    if (status) return status;
    int current = dup(root_fd);
    if (current < 0) return 500;
    char *component = path;
    const char *filename = "index.html";
    /* Walk relative to already opened directories, never concatenate a root
     * with user input. O_NOFOLLOW rejects symlinks at every component.
     */
    while (*component) {
        char *slash = strchr(component, '/');
        if (slash) *slash = '\0';
        if (*component == '.' || !*component) {
            close(current); return 403;
        }
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
        if (slash) flags |= O_DIRECTORY;
        int next = openat(current, component, flags);
        if (next < 0) {
            status = error_status(errno);
            close(current); return status;
        }
        close(current);
        current = next;
        filename = component;
        if (!slash) break;
        component = slash + 1;
    }
    struct stat metadata;
    if (fstat(current, &metadata) < 0) {
        close(current); return 500;
    }
    if (S_ISDIR(metadata.st_mode)) {
        int index = openat(current, "index.html", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (index < 0) {
            status = error_status(errno);
            close(current); return status;
        }
        close(current); current = index; filename = "index.html";
        if (fstat(current, &metadata) < 0) {
            close(current); return 500;
        }
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0) {
        close(current); return 403;
    }
    int n = snprintf(file->etag, sizeof file->etag, "W/\"%jx-%jx-%jx-%jx-%jx\"",
        (uintmax_t)metadata.st_dev, (uintmax_t)metadata.st_ino,
        (uintmax_t)metadata.st_size, (uintmax_t)metadata.st_mtim.tv_sec,
        (uintmax_t)metadata.st_mtim.tv_nsec);
    struct tm utc;
    if (n < 0 || (size_t)n >= sizeof file->etag ||
        !gmtime_r(&metadata.st_mtime, &utc) ||
        !strftime(file->last_modified, sizeof file->last_modified, "%a, %d %b %Y %H:%M:%S GMT", &utc)) {
        close(current); return 500;
    }
    file->data = NULL; file->stamp = metadata;
    file->fd = current;
    file->length = metadata.st_size;
    file->type = mime_type(filename);
    return 0;
}
