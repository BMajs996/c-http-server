#ifndef STATIC_FILES_H
#define STATIC_FILES_H
#include <sys/types.h>
#include <sys/stat.h>
struct static_file { int fd; off_t length; const char *type; char etag[160], last_modified[64]; char *data; struct stat stamp; };
/* Prepare a regular file without writing to the socket. Caller owns fd. */
int prepare_static(int root_fd, const char *target, struct static_file *file);
#endif
