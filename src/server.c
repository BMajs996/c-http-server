/* Linux epoll HTTP server. Build: make
 * Run: ./http_server [port] [document-root] [max-connections] [shutdown-ms]
 * Defaults: 8080, public, 1024, 5000. Binds to localhost.
 */
#define _GNU_SOURCE
#include "connection.h"
#include "logging.h"
#include "file_io.h"
#include "metrics.h"
#include "config.h"
#include "file_cache.h"
#include "transport.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Signal handler requires lock-free atomic int");
atomic_int stopping = 0;
static atomic_int shutdown_requests, reload_requested;
static void reload_credentials(int sig) { (void)sig;atomic_store(&reload_requested,1); }
static void stop_server(int sig) { (void)sig; atomic_fetch_add(&shutdown_requests, 1); }
static int64_t now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static long number(const char *text, long maximum) {
    char *end;
    errno = 0;
    long n = strtol(text, &end, 10);
    if (errno || end == text || *end || n < 1 || n > maximum) return -1;
    return n;
}
static void remove_connection(int epoll_fd, connection **slot) {
    (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, connection_fd(*slot), NULL);
    connection_destroy(*slot);
    *slot = NULL;
}
static void advance(int epoll_fd, connection **slot, unsigned index) {
    uint32_t interest = connection_step(*slot);
    struct epoll_event event = {.events = interest & ~CONNECTION_WAIT_IO, .data.u32 = index};
    if (!interest || epoll_ctl(epoll_fd, EPOLL_CTL_MOD, connection_fd(*slot), &event) < 0)
        remove_connection(epoll_fd, slot);
}
int main(int argc, char **argv) {
    config_defaults();
    if (argc >= 2 && !strcmp(argv[1], "--config")) {
        if (argc != 3 || config_load(argv[2]) < 0) return EXIT_FAILURE;
    } else {
        long port_value = argc >= 2 ? number(argv[1], 65535) : config.port;
        long limit_value = argc >= 4 ? number(argv[3], 16384) : config.max_connections;
        long grace_value = argc >= 5 ? number(argv[4], 60000) : config.shutdown_ms;
        if (argc > 5 || port_value < 0 || limit_value < 0 || grace_value < 0 ||
            (argc >= 3 && strlen(argv[2]) >= sizeof config.document_root)) {
            fprintf(stderr, "Usage: %s [port] [document-root] [max-connections] [shutdown-ms]\n       %s --config server.conf\n", argv[0], argv[0]);
            return EXIT_FAILURE;
        }
        config.port=(unsigned)port_value; config.max_connections=(unsigned)limit_value;
        config.shutdown_ms=(unsigned)grace_value;
        if(argc>=3)strcpy(config.document_root,argv[2]);
    }
    long port=config.port, limit=config.max_connections, grace_ms=config.shutdown_ms;
    /* Install signals before any thread starts. */
    struct sigaction action = {0};
    action.sa_handler = stop_server;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0 ||
        signal(SIGPIPE, SIG_IGN) == SIG_ERR) { perror("signal"); return EXIT_FAILURE; }
    action.sa_handler=reload_credentials;
    if(sigaction(SIGHUP,&action,NULL)<0){perror("signal");return EXIT_FAILURE;}
    if(transport_init()<0)return EXIT_FAILURE;
    if (logging_init() < 0) {transport_close();return EXIT_FAILURE;}
    metrics_init((unsigned)limit);
    int root = -1, server = -1, epoll_fd = -1, failed = 1;
    connection **slots = NULL;
    root = open(config.document_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root < 0) { perror("document root"); goto cleanup; }
    if(auth_init()<0){fprintf(stderr,"Authentication initialization failed: check credential file and limits\n");goto cleanup;}
    server = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server < 0) { perror("socket"); goto cleanup; }
    int reuse = 1;
    if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) < 0) {
        perror("setsockopt"); goto cleanup;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, (struct sockaddr *)&address, sizeof address) < 0 || listen(server, 256) < 0) {
        perror("bind/listen"); goto cleanup;
    }
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) { perror("epoll_create1"); goto cleanup; }
    if (file_io_init() < 0) { fprintf(stderr, "file I/O pool initialization failed\n"); goto cleanup; }
    struct epoll_event completion = {.events = EPOLLIN, .data.u32 = UINT32_MAX - 1};
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, file_io_eventfd(), &completion) < 0) {
        perror("I/O completion epoll registration"); goto cleanup;
    }
    slots = calloc((size_t)limit, sizeof *slots);
    if (!slots) { perror("calloc"); goto cleanup; }
    /* Slots are stable IDs. UINT32_MAX identifies the listener, never a client. */
    struct epoll_event listener = {.events = EPOLLIN, .data.u32 = UINT32_MAX};
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server, &listener) < 0) {
        perror("epoll_ctl"); goto cleanup;
    }
    printf("Listening on %s://127.0.0.1:%ld (epoll; max %ld connections; Ctrl+C to stop)\n", transport_tls_enabled()?"https":"http",port, limit);
    fflush(stdout);
    failed = 0;
    int draining = 0;
    int64_t drain_deadline = 0;
    io_job *credential_reload=NULL;int reload_pending=0;
    while (!stopping) {
        if (atomic_load(&shutdown_requests) && !draining) {
            draining = 1; metrics_draining();
            int64_t now = now_ms();
            if (now < 0) { failed = 1; break; }
            drain_deadline = now + grace_ms;
            (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, server, NULL);
            close(server); server = -1;
            for (unsigned i = 0; i < (unsigned)limit; ++i) if (slots[i]) {
                connection_drain(slots[i]); advance(epoll_fd, &slots[i], i);
            }
        }
        if (draining && (metrics_active() == 0 || atomic_load(&shutdown_requests) > 1 || now_ms() >= drain_deadline)) break;
        if(atomic_exchange(&reload_requested,0))reload_pending=1;
        if(reload_pending && !credential_reload && !draining) {
            if(!*config.auth_credentials_file){auth_reload_result(NULL);reload_pending=0;}
            else {credential_reload=file_io_auth_reload();if(credential_reload)reload_pending=0;}
        }
        struct epoll_event events[128];
        int count = epoll_wait(epoll_fd, events, 128, 100);
        if (count < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait"); failed = 1; break;
        }
        for (int i = 0; i < count && !stopping; ++i) {
            unsigned index = events[i].data.u32;
            if (index == UINT32_MAX) {
                if (server < 0 || atomic_load(&shutdown_requests)) continue;
                /* Bound accept work too, so a connection flood cannot starve
                 * existing clients. Resource exhaustion exits cleanly rather
                 * than spinning on a perpetually readable listener. */
                for (unsigned accepted = 0; accepted < 64; ++accepted) {
                    if (atomic_load(&shutdown_requests)) break;
                    int client = accept4(server, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (client < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR || errno == ECONNABORTED) continue;
                        perror("accept4"); failed = 1; stopping = 1; break;
                    }
                    metrics_accept();
                    /* Small keep-alive responses otherwise interact with
                     * Nagle/delayed ACK and can incur tens of ms of latency. */
                    int no_delay = 1;
                    if (setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof no_delay) < 0) {
                        metrics_reject(); close(client); continue;
                    }
                    unsigned slot = 0;
                    while (slot < (unsigned)limit && slots[slot]) ++slot;
                    if (slot == (unsigned)limit) { metrics_reject(); close(client); continue; }
                    connection *c = connection_create(client, root);
                    if (!c) { metrics_reject(); close(client); continue; }
                    struct epoll_event event = {.events = EPOLLIN, .data.u32 = slot};
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client, &event) < 0) {
                        metrics_reject(); connection_destroy(c); continue;
                    }
                    slots[slot] = c;
                }
            } else if (index == UINT32_MAX - 1) {
                file_io_ack();
            } else if (index < (unsigned)limit && slots[index]) {
                if (events[i].events & (EPOLLERR | EPOLLHUP)) remove_connection(epoll_fd, &slots[index]);
                else advance(epoll_fd, &slots[index], index);
            }
        }
        /* Apply completions only after the batch, so callbacks cannot free a
         * slot that still has a stale socket event in this batch. */
        io_job *job;
        while ((job = file_io_result())) {
            if(job->kind==2) {
                credential_reload=NULL;
                if(!draining){auth_reload_result(job->credentials);job->credentials=NULL;}
                file_io_release(job);continue;
            }
            connection *owner = job->owner;
            if (owner) connection_io_complete(owner, job);
            file_io_release(job);
            if (owner) {
                for (unsigned i = 0; i < (unsigned)limit; ++i)
                    if (slots[i] == owner) { advance(epoll_fd, &slots[i], i); break; }
            }
        }
        /* Expiry sweep happens after the whole batch: slots cannot be freed
         * by timers while a batch still contains events for those slots. */
        int64_t now = now_ms();
        if (now < 0) { failed = 1; break; }
        for (unsigned i = 0; i < (unsigned)limit; ++i)
            if (slots[i] && (connection_expired(slots[i], now) || connection_waiting(slots[i]))) advance(epoll_fd, &slots[i], i);
    }
cleanup:
    stopping = 1;
    if (slots) {
        for (long i = 0; i < limit; ++i) if (slots[i]) connection_destroy(slots[i]);
        free(slots);
    }
    file_io_close();
    auth_close();
    file_cache_close(); /* Join active disk work before closing the shared root. */
    if (epoll_fd >= 0) close(epoll_fd);
    if (server >= 0) close(server);
    if (root >= 0) close(root);
    logging_close();
    transport_close();
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
