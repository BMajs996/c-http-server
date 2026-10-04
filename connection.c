#define _POSIX_C_SOURCE 200809L
#include "connection.h"
#include "logging.h"
#include "metrics.h"
#include "config.h"
#include "file_cache.h"
#include "transport.h"
#include "cache.h"
#include "http_parser.h"
#include "router.h"
#include "static_files.h"
#include <errno.h>
#include <inttypes.h>
#include <sys/epoll.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

extern atomic_int stopping;
#define REQUEST_TIMEOUT_MS config.request_timeout_ms
#define RESPONSE_TIMEOUT_MS config.response_timeout_ms
#define MAX_REQUESTS config.max_requests

typedef enum {
    READING_HEADERS, READING_BODY, VALIDATING_REQUEST, PREPARING_RESPONSE,
    WRITING_HEADERS, WRITING_BODY, RESPONSE_COMPLETE, CLOSING, WAIT_FILE_OPEN, WAIT_FILE_READ, TLS_HANDSHAKE, TLS_SHUTDOWN
} connection_state;

struct connection {
    int fd, root_fd, file_fd;
    connection_state state;
    struct http_request request;
    char input[HEADER_LIMIT + 1];
    size_t input_used, scan_from, request_length;
    char output[1536], chunk[32768];
    size_t output_length, output_offset, inline_body_start, chunk_length, chunk_offset;
    const char *body, *type, *allow;
    char *request_body;
    size_t body_used;
    uintmax_t body_length, remaining;
    int status, keep_alive;
    unsigned completed;
    int64_t deadline;
    uint32_t interest;
    int64_t started;
    uintmax_t sent;
    int logged;
    char extra_headers[768];
    io_job *job;
    struct static_file opened;
    int open_ready;
    off_t file_offset;
    int draining, timed_out;
    char metrics_body[8192];
    file_cache_entry *cached;
    int sendfile_disabled;
    transport *socket_transport;
};

static int64_t now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 304: return "Not Modified";
    case 416: return "Range Not Satisfiable";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default: return "Internal Server Error";
    }
}
static void error_response(connection *c, int status) {
    c->status = status;
    c->body = reason(status);
    c->body_length = strlen(c->body);
    c->type = "text/plain; charset=utf-8";
    if(route_is_api(c->request.target)) {
        route_response response={0};route_error(status,&response);
        c->status=response.status;c->body=response.body;
        c->body_length=response.length;c->type=response.type;
    }
}
static void read_error(connection *c, int status) {
    if (status == 408 && !c->timed_out) { metrics_timeout(); c->timed_out = 1; }
    c->keep_alive = 0;
    error_response(c, status);
    c->state = PREPARING_RESPONSE;
}
static void read_headers(connection *c) {
    /* Search raw bytes: never let an embedded NUL hide a terminator. */
    for (size_t i = c->scan_from; i + 3 < c->input_used; ++i) {
        if (!memcmp(c->input + i, "\r\n\r\n", 4)) {
            c->request_length = i + 4;
            c->state = VALIDATING_REQUEST;
            return;
        }
    }
    c->scan_from = c->input_used > 3 ? c->input_used - 3 : 0;
    if (c->input_used == HEADER_LIMIT) { read_error(c, 431); return; }
    ssize_t n = transport_read(c->socket_transport,c->input+c->input_used,HEADER_LIMIT-c->input_used,&c->interest);
    if (n > 0) {
        if (!c->input_used) c->started = now_ms();
        c->input_used += (size_t)n; return;
    }
    if(n==-2)return;
    c->state=CLOSING;
}
static void validate(connection *c) {
    int status = parse_request(c->input, c->request_length, &c->request);
    size_t left = c->input_used - c->request_length;
    memmove(c->input, c->input + c->request_length, left);
    c->input_used = left;
    c->scan_from = 0;
    if (!status) status = route_check(&c->request,&c->allow);
    c->keep_alive = status == 0 && !c->draining && c->request.keep_alive && c->completed + 1 < MAX_REQUESTS;
    if (status) { metrics_request_rejected(); error_response(c, status); }
    if(!status && c->request.content_length) {
        c->request_body=malloc(c->request.content_length);
        if(!c->request_body){c->keep_alive=0;error_response(c,500);}
        else {c->state=READING_BODY;return;}
    }
    c->state = PREPARING_RESPONSE;
}
static void read_body(connection *c) {
    size_t remaining=c->request.content_length-c->body_used;
    if(c->input_used) {
        size_t count=c->input_used<remaining?c->input_used:remaining;
        memcpy(c->request_body+c->body_used,c->input,count);c->body_used+=count;
        c->input_used-=count;memmove(c->input,c->input+count,c->input_used);
    } else {
        ssize_t n=transport_read(c->socket_transport,c->request_body+c->body_used,remaining,&c->interest);
        if(n>0)c->body_used+=(size_t)n;
        else if(n!=-2){c->state=CLOSING;return;}
    }
    if(c->body_used==c->request.content_length)c->state=PREPARING_RESPONSE;
}
static void prepare(connection *c) {
    if (!c->status) {
        char *query = strchr(c->request.target, '?');
        if (query) *query = 0;
        route_response response;
        if(route_dispatch(&c->request,c->request_body?c->request_body:"",c->metrics_body,sizeof c->metrics_body,&response)) {
            c->status=response.status;c->body=response.body;c->body_length=response.length;
            c->type=response.type;c->allow=response.allow;
        } else {
            if (!c->open_ready) {
                int gzip=c->request.gzip_q>0 && c->request.gzip_q>=c->request.identity_q;
                c->cached=(gzip || c->request.identity_q>0)?
                    file_cache_get_variant(c->request.target,gzip):NULL;
                if(c->cached){c->opened=file_cache_metadata(c->cached);c->open_ready=1;}
            }
            if (!c->open_ready) {
                c->job = file_io_open(c, c->root_fd, c->request.target, c->request.gzip_q, c->request.identity_q);
                c->state = WAIT_FILE_OPEN;
                c->interest = CONNECTION_WAIT_IO;
                return;
            }
            struct static_file file = c->opened;
            int status = c->open_ready == 1 ? 0 : c->open_ready;
            c->opened.fd = -1; c->opened.data=NULL;
            if(!status && file.data) {
                c->cached=file_cache_insert(c->request.target,&file);
                free(file.data);file.data=NULL;
                if(c->cached){close(file.fd);file.fd=-1;}
            }
            if (status) {
                error_response(c,status);
                if(status==406)strcpy(c->extra_headers,"Vary: Accept-Encoding\r\n");
            } else {
                c->status = 200; c->file_fd = file.fd;
                c->body_length = (uintmax_t)file.length; c->type = file.type;
                int n = snprintf(c->extra_headers, sizeof c->extra_headers,
                    "ETag: %s\r\nLast-Modified: %s\r\nCache-Control: no-cache\r\nVary: Accept-Encoding\r\nAccept-Ranges: %s\r\n%s",
                    file.etag, file.last_modified,file.gzip?"none":"bytes",
                    file.gzip?"Content-Encoding: gzip\r\n":"");
                if (n < 0 || (size_t)n >= sizeof c->extra_headers) { c->state = CLOSING; return; }
                if (*c->request.if_none_match && etag_matches(c->request.if_none_match, file.etag)) {
                    c->status = 304;
                } else if (!file.gzip && !c->request.head && *c->request.range && !*c->request.if_range) {
                    /* Our metadata ETag is weak; If-Range cannot safely use it.
                     * Conservatively return the full representation whenever
                     * If-Range is supplied, including date validators. */
                    uintmax_t start, length, size = c->body_length;
                    int range = byte_range(c->request.range, size, &start, &length);
                    if (range < 0) {
                        error_response(c, 416);
                        snprintf(c->extra_headers + n, sizeof c->extra_headers - (size_t)n,
                                 "Content-Range: bytes */%" PRIuMAX "\r\n", size);
                        close(c->file_fd); c->file_fd = -1;
                    } else if (range > 0) {
                        c->file_offset = (off_t)start;
                        c->status = 206; c->body_length = length;
                        snprintf(c->extra_headers + n, sizeof c->extra_headers - (size_t)n,
                                 "Content-Range: bytes %" PRIuMAX "-%" PRIuMAX "/%" PRIuMAX "\r\n",
                                 start, start + length - 1, size);
                    }
                }
            }
        }
    }
    if(route_is_api(c->request.target))strcpy(c->extra_headers,"Cache-Control: no-store\r\n");
    if(c->status==405)snprintf(c->extra_headers,sizeof c->extra_headers,"%sAllow: %s\r\n",route_is_api(c->request.target)?"Cache-Control: no-store\r\n":"",c->allow?c->allow:"GET, HEAD");
    int n = snprintf(c->output, sizeof c->output,
        "%s %d %s\r\nContent-Type: %s\r\nContent-Length: %" PRIuMAX "\r\n"
        "X-Content-Type-Options: nosniff\r\nConnection: %s\r\n%s\r\n",
        c->request.http11 ? "HTTP/1.1" : "HTTP/1.0", c->status, reason(c->status),
        c->type, c->body_length, c->keep_alive ? "keep-alive" : "close",
        c->extra_headers);
    if (n < 0 || (size_t)n >= sizeof c->output) { c->state = CLOSING; return; }
    c->output_length = (size_t)n; c->output_offset = 0;
    c->remaining = c->request.head || c->status == 304 ? 0 : c->body_length;
    c->inline_body_start = 0;
    /* Small generated bodies fit beside the headers, avoiding a second write.
     * Keep this buffer stable across partial writes, including TLS retries. */
    if(c->remaining && c->body && c->file_fd<0 &&
       c->remaining<sizeof c->output-c->output_length) {
        c->inline_body_start=c->output_length;
        memcpy(c->output+c->output_length,c->body,(size_t)c->remaining);
        c->output_length+=(size_t)c->remaining;c->remaining=0;
        c->output[c->output_length]=0;
    }
    int64_t now = now_ms();
    if (now < 0) { c->state = CLOSING; return; }
    c->deadline = now + RESPONSE_TIMEOUT_MS;
    c->state = WRITING_HEADERS;
}
/* Returns 1 when the buffer finishes, 0 when more work remains, -1 on failure. */
static int write_buffer(connection *c, const char *buffer, size_t length, size_t *offset) {
    if (*offset == length) return 1;
    ssize_t n = transport_write(c->socket_transport,buffer+*offset,length-*offset,&c->interest);
    if (n > 0) { *offset += (size_t)n; return *offset == length; }
    return n==-2?0:-1;
}
static void write_body(connection *c) {
    if (!c->remaining) { c->state = RESPONSE_COMPLETE; return; }
    if(c->file_fd>=0 && !c->sendfile_disabled &&
       c->chunk_offset==c->chunk_length && (c->status==200 || c->status==206)) {
        /* Bound each reactor turn. sendfile advances file_offset only for
         * bytes actually sent, so a partial write resumes at the right byte. */
        size_t wanted=c->remaining<65536 ? (size_t)c->remaining : 65536;
        ssize_t n=transport_sendfile(c->socket_transport,c->file_fd,
                                     &c->file_offset,wanted,&c->interest);
        if(n>0) {
            c->remaining-=(size_t)n;c->sent+=(size_t)n;
            return;
        }
        if(n==-2)return;
        if(n==-3)c->sendfile_disabled=1;
        else {c->state=CLOSING;return;}
    }
    if (c->chunk_offset == c->chunk_length) {
        c->chunk_offset = 0;
        size_t wanted = c->remaining < sizeof c->chunk ? (size_t)c->remaining : sizeof c->chunk;
        if(c->cached && (c->status==200 || c->status==206)) {
            memcpy(c->chunk,file_cache_data(c->cached)+(size_t)c->file_offset,wanted);
            c->file_offset+=(off_t)wanted;c->chunk_length=wanted;
        } else if (c->file_fd >= 0) {
            c->job = file_io_read(c, c->file_fd, c->file_offset, wanted);
            c->state = WAIT_FILE_READ;
            c->interest = CONNECTION_WAIT_IO;
            return;
        } else {
            memcpy(c->chunk, c->body + (size_t)(c->body_length - c->remaining), wanted);
            c->chunk_length = wanted;
        }
    }
    size_t before = c->chunk_offset;
    int rc = write_buffer(c, c->chunk, c->chunk_length, &c->chunk_offset);
    c->remaining -= c->chunk_offset - before;
    c->sent += c->chunk_offset - before;
    if (rc < 0) c->state = CLOSING;
}
static void complete(connection *c) {
    int64_t elapsed=now_ms()-c->started;
    metrics_response(c->status, c->sent, 1, elapsed);
    log_request(c->fd, c->completed + 1, c->request.target, c->request.method,
                c->status, c->sent, elapsed, 1);
    c->logged = 1;
    file_cache_release(c->cached);c->cached=NULL;
    if (c->file_fd >= 0) { close(c->file_fd); c->file_fd = -1; }
    free(c->request_body);c->request_body=NULL;c->body_used=0;c->allow=NULL;
    ++c->completed;
    if (!c->keep_alive) { c->state = transport_tls_enabled()?TLS_SHUTDOWN:CLOSING; return; }
    c->sent = 0; c->logged = 0; c->timed_out = 0;
    c->extra_headers[0] = 0;
    c->open_ready = 0; c->file_offset = 0;
    c->sendfile_disabled = 0;
    c->status = 0; c->body = NULL; c->type = NULL;
    c->body_length = c->remaining = 0;
    c->chunk_offset = c->chunk_length = 0;
    memset(&c->request, 0, sizeof c->request);
    int64_t now = now_ms();
    if (now < 0) { c->state = CLOSING; return; }
    c->deadline = now + REQUEST_TIMEOUT_MS;
    c->started = now;
    c->state = READING_HEADERS;
}

connection *connection_create(int socket_fd, int root_fd) {
    connection *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd = socket_fd; c->root_fd = root_fd; c->file_fd = -1; c->opened.fd = -1;
    c->socket_transport=transport_create(socket_fd);
    if(!c->socket_transport){free(c);return NULL;}
    c->state = transport_tls_enabled()?TLS_HANDSHAKE:READING_HEADERS;
    int64_t now = now_ms();
    if (now < 0) { transport_destroy(c->socket_transport);free(c); return NULL; }
    c->deadline = now + REQUEST_TIMEOUT_MS;
    c->started = now;
    metrics_open();
    return c;
}
int connection_fd(const connection *c) { return c->fd; }
int connection_expired(const connection *c, int64_t now) { return now >= c->deadline; }
uint32_t connection_step(connection *c) {
    c->interest = 0;
    /* A bounded turn prevents a large transfer or pipeline monopolizing the
     * reactor. If CPU work remains, writable readiness schedules another turn.
     * EAGAIN returns immediately to epoll; never wait inside this FSM.
     */
    for (unsigned turn = 0; turn < 64 && c->state != CLOSING && !stopping; ++turn) {
        int64_t now = now_ms();
        if (now < 0) return 0;
        if (now >= c->deadline) {
            if ((c->state == READING_HEADERS && c->input_used) || c->state==READING_BODY) read_error(c, 408);
            else if (c->state != PREPARING_RESPONSE) {
                if (!c->timed_out) { metrics_timeout(); c->timed_out = 1; }
                return 0;
            }
        }
        switch (c->state) {
        case TLS_HANDSHAKE: {
            int result=transport_handshake(c->socket_transport,&c->interest);
            if(result<0)c->state=CLOSING;
            else if(result>0){c->state=READING_HEADERS;c->deadline=now+REQUEST_TIMEOUT_MS;}
            break;
        }
        case TLS_SHUTDOWN: {
            int result=transport_shutdown(c->socket_transport,&c->interest);
            if(result!=0)c->state=CLOSING;
            break;
        }
        case READING_HEADERS: read_headers(c); break;
        case READING_BODY: read_body(c); break;
        case VALIDATING_REQUEST: validate(c); break;
        case PREPARING_RESPONSE: prepare(c); break;
        case WRITING_HEADERS: {
            size_t before=c->output_offset;
            int rc = write_buffer(c, c->output, c->output_length, &c->output_offset);
            if(c->inline_body_start && c->output_offset>c->inline_body_start) {
                size_t start=before>c->inline_body_start?before:c->inline_body_start;
                c->sent+=c->output_offset-start;
            }
            if (rc < 0) c->state = CLOSING;
            else if (rc > 0) c->state = WRITING_BODY;
            break;
        }
        case WRITING_BODY: write_body(c); break;
        case RESPONSE_COMPLETE: complete(c); break;
        case WAIT_FILE_OPEN:
            if (!c->job) c->job = file_io_open(c, c->root_fd, c->request.target, c->request.gzip_q, c->request.identity_q);
            c->interest = CONNECTION_WAIT_IO; break;
        case WAIT_FILE_READ:
            if (!c->job) {
                size_t wanted = c->remaining < sizeof c->chunk ? (size_t)c->remaining : sizeof c->chunk;
                c->job = file_io_read(c, c->file_fd, c->file_offset, wanted);
            }
            c->interest = CONNECTION_WAIT_IO; break;
        case CLOSING: break;
        }
        if (c->state == CLOSING) return 0;
        if (c->interest) return c->interest;
    }
    return c->state == CLOSING || stopping ? 0 : EPOLLOUT;
}
void connection_destroy(connection *c) {
    if (!c) return;
    if (c->job) c->job->owner = NULL; /* Orphan result is released by reactor. */
    free(c->request_body);
    free(c->opened.data);
    file_cache_release(c->cached);
    if (c->opened.fd >= 0) close(c->opened.fd);
    if (!c->logged && c->status) {
        int64_t elapsed=now_ms()-c->started;
        metrics_response(c->status, c->sent, 0, elapsed);
        log_request(c->fd, c->completed + 1, c->request.target, c->request.method,
                    c->status, c->sent, elapsed, 0);
    }
    metrics_close();
    if (c->file_fd >= 0) close(c->file_fd);
    transport_destroy(c->socket_transport);
    close(c->fd);
    free(c);
}

void connection_io_complete(connection *c, io_job *job) {
    c->job = NULL;
    int64_t now = now_ms();
    if (now < 0 || now >= c->deadline) {
        if (now >= c->deadline && !c->timed_out) { metrics_timeout(); c->timed_out = 1; }
        c->state = CLOSING; return;
    }
    if (c->state == WAIT_FILE_OPEN) {
        c->open_ready = job->status ? job->status : 1;
        if (!job->status) { c->opened = job->file; job->file.fd = -1;job->file.data=NULL; }
        c->state = PREPARING_RESPONSE;
    } else if (c->state == WAIT_FILE_READ) {
        if (job->bytes <= 0) c->state = CLOSING;
        else {
            memcpy(c->chunk, job->data, (size_t)job->bytes);
            c->chunk_length = (size_t)job->bytes; c->chunk_offset = 0;
            c->file_offset += job->bytes;
            c->state = WRITING_BODY;
        }
    }
}
int connection_waiting(const connection *c) {
    return (c->state == WAIT_FILE_OPEN || c->state == WAIT_FILE_READ) && !c->job;
}

void connection_drain(connection *c) {
    c->draining = 1; c->keep_alive = 0;
    if (c->state == READING_HEADERS || c->state == READING_BODY || c->state == TLS_HANDSHAKE) { c->state = CLOSING; return; }
    /* Never rewrite a header after sending any of its bytes. Persistence can
     * end at the response boundary even if keep-alive was already advertised. */
    if (c->state == WRITING_HEADERS && c->output_offset == 0) {
        char *field = strstr(c->output, "Connection: keep-alive\r\n");
        if (field) {
            const char *replacement = "Connection: close\r\n";
            size_t old = strlen("Connection: keep-alive\r\n"), length = strlen(replacement);
            size_t at = (size_t)(field - c->output);
            memmove(field + length, field + old, c->output_length - at - old);
            memcpy(field, replacement, length);
            c->output_length -= old - length;
            if(c->inline_body_start)c->inline_body_start-=old-length;
        }
    }
}
