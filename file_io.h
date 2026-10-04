#ifndef FILE_IO_H
#define FILE_IO_H
#include "static_files.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef struct io_job {
    struct io_job *next;
    void *owner; /* Reactor-only: workers never dereference or modify owner. */
    int kind, source_fd, status, gzip_q, identity_q;
    off_t offset;
    size_t length;
    char target[2048], data[32768];
    struct static_file file;
    ssize_t bytes;
} io_job;
struct file_io_stats { unsigned outstanding, queued, running, completed_pending, limit; uint64_t submitted, finished; };
void file_io_snapshot(struct file_io_stats *stats);
int file_io_init(void);
int file_io_eventfd(void);
io_job *file_io_open(void *owner, int root, const char *target, int gzip_q, int identity_q);
io_job *file_io_read(void *owner, int fd, off_t offset, size_t length);
/* Reactor consumes completion wakeup then drains results. */
void file_io_ack(void);
io_job *file_io_result(void);
void file_io_release(io_job *job);
void file_io_close(void);
#endif
