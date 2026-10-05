#define _POSIX_C_SOURCE 200809L
#include "file_io.h"
#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
#define IO_WORKERS config.file_workers
#define IO_WORKER_STORAGE 16
#define IO_LIMIT config.file_job_limit
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static pthread_t workers[IO_WORKER_STORAGE];
static unsigned count, outstanding, running;
static uint64_t submitted, finished;
static int closing, wake_fd = -1;
static long test_delay;
static io_job *pending_head, *pending_tail, *done_head, *done_tail;
static void append(io_job **head, io_job **tail, io_job *job) {
    job->next = NULL;
    if (*tail) (*tail)->next = job; else *head = job;
    *tail = job;
}
static io_job *pop(io_job **head, io_job **tail) {
    io_job *job = *head;
    if (job) { *head = job->next; if (!*head) *tail = NULL; }
    return job;
}
static void *worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!pending_head && !closing) pthread_cond_wait(&ready, &lock);
        if (closing) { pthread_mutex_unlock(&lock); break; }
        io_job *job = pop(&pending_head, &pending_tail);
        ++running;
        pthread_mutex_unlock(&lock);
        if (test_delay) {
            struct timespec delay = {test_delay / 1000, (test_delay % 1000) * 1000000};
            (void)nanosleep(&delay, NULL);
        }
        if (job->kind == 0) {
            job->status = prepare_static(job->source_fd, job->target, job->gzip_q, job->identity_q, &job->file);
            if(!job->status && config.cache_bytes && (uintmax_t)job->file.length<=config.cache_max_file_bytes) {
                size_t length=(size_t)job->file.length;
                char *data=malloc(length+1);
                if(data) {
                    size_t used=0;
                    while(used<length) {
                        ssize_t n=pread(job->file.fd,data+used,length-used,(off_t)used);
                        if(n<0 && errno==EINTR)continue;
                        if(n<=0)break;
                        used+=(size_t)n;
                    }
                    struct stat after,before=job->file.stamp;
                    if(used==length && fstat(job->file.fd,&after)==0 &&
                       before.st_size==after.st_size && before.st_mtim.tv_sec==after.st_mtim.tv_sec &&
                       before.st_mtim.tv_nsec==after.st_mtim.tv_nsec && before.st_ctim.tv_sec==after.st_ctim.tv_sec &&
                       before.st_ctim.tv_nsec==after.st_ctim.tv_nsec)job->file.data=data;
                    else free(data);
                }
            }
        }
        else if(job->kind==2) {
            job->credentials=auth_load(job->target,config.document_root);
        } else {
            do { job->bytes = pread(job->source_fd, job->data, job->length, job->offset); }
            while (job->bytes < 0 && errno == EINTR);
            close(job->source_fd); job->source_fd = -1;
        }
        pthread_mutex_lock(&lock);
        --running; ++finished;
        append(&done_head, &done_tail, job);
        pthread_mutex_unlock(&lock);
        uint64_t one = 1;
        ssize_t written;
        do { written = write(wake_fd, &one, sizeof one); } while (written < 0 && errno == EINTR);
        /* EAGAIN means a wakeup is already pending (counter saturation). */
    }
    return NULL;
}
int file_io_init(void) {
    const char *delay = getenv("C_HTTP_TEST_IO_DELAY_MS");
    if (delay) {
        char *end; test_delay = strtol(delay, &end, 10);
        if (*end || test_delay < 0 || test_delay > 5000) return -1;
    }
    wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd < 0) return -1;
    for (; count < IO_WORKERS; ++count) {
        if (pthread_create(&workers[count], NULL, worker, NULL) != 0) {
            file_io_close(); return -1;
        }
    }
    return 0;
}
int file_io_eventfd(void) { return wake_fd; }
static io_job *submit(void *owner, int fd, const char *target, off_t offset, size_t length, int gzip_q, int identity_q, int kind) {
    pthread_mutex_lock(&lock);
    if (closing || outstanding == IO_LIMIT) { pthread_mutex_unlock(&lock); return NULL; }
    ++outstanding;
    pthread_mutex_unlock(&lock);
    io_job *job = calloc(1, sizeof *job);
    if (!job) goto failed;
    job->gzip_q=gzip_q;job->identity_q=identity_q;
    job->owner = owner; job->file.fd = -1; job->source_fd = fd;
    job->kind=kind;
    if (target) { strcpy(job->target, target); }
    else {
        job->kind = 1; job->length = length; job->offset = offset;
        job->source_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (job->source_fd < 0) { free(job); goto failed; }
    }
    pthread_mutex_lock(&lock);
    ++submitted;
    append(&pending_head, &pending_tail, job);
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&lock);
    return job;
failed:
    pthread_mutex_lock(&lock); --outstanding; pthread_mutex_unlock(&lock);
    return NULL;
}
io_job *file_io_open(void *owner, int root, const char *target,int gzip_q,int identity_q) {
    if (strlen(target) >= sizeof ((io_job *)0)->target) return NULL;
    return submit(owner, root, target, 0, 0, gzip_q, identity_q, 0);
}
io_job *file_io_read(void *owner, int fd, off_t offset, size_t length) {
    if (length > sizeof ((io_job *)0)->data) return NULL;
    return submit(owner, fd, NULL, offset, length, 0, 0, 1);
}
io_job *file_io_auth_reload(void) {
    return submit(NULL,-1,config.auth_credentials_file,0,0,0,0,2);
}
void file_io_ack(void) {
    uint64_t count_value;
    while (read(wake_fd, &count_value, sizeof count_value) > 0) {}
}
io_job *file_io_result(void) {
    pthread_mutex_lock(&lock);
    io_job *job = pop(&done_head, &done_tail);
    pthread_mutex_unlock(&lock);
    return job;
}
void file_io_release(io_job *job) {
    if (!job) return;
    auth_keyset_free(job->credentials);
    free(job->file.data);
    if (job->file.fd >= 0) close(job->file.fd);
    if (job->kind == 1 && job->source_fd >= 0) close(job->source_fd);
    free(job);
    pthread_mutex_lock(&lock); --outstanding; pthread_mutex_unlock(&lock);
}
void file_io_close(void) {
    pthread_mutex_lock(&lock); closing = 1; pthread_cond_broadcast(&ready); pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < count; ++i) pthread_join(workers[i], NULL);
    count = 0;
    io_job *job;
    while ((job = pop(&pending_head, &pending_tail))) file_io_release(job);
    while ((job = pop(&done_head, &done_tail))) file_io_release(job);
    if (wake_fd >= 0) close(wake_fd);
    wake_fd = -1;
}

void file_io_snapshot(struct file_io_stats *stats) {
    pthread_mutex_lock(&lock);
    *stats = (struct file_io_stats){.outstanding = outstanding, .running = running,
        .limit = IO_LIMIT, .submitted = submitted, .finished = finished};
    for (io_job *job = pending_head; job; job = job->next) ++stats->queued;
    for (io_job *job = done_head; job; job = job->next) ++stats->completed_pending;
    pthread_mutex_unlock(&lock);
}
