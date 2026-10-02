#ifndef CONNECTION_H
#define CONNECTION_H
#include <stdint.h>
#include "file_io.h"
#define CONNECTION_WAIT_IO UINT32_C(0x80000000)
typedef struct connection connection;
/* Single reactor owns each connection. create takes socket ownership only on
 * success; destroy releases its socket, file descriptor, and buffers. */
connection *connection_create(int socket_fd, int root_fd);
int connection_fd(const connection *c);
int connection_expired(const connection *c, int64_t now);
/* Advance bounded work; EPOLLIN/EPOLLOUT means wait/resume, 0 means close. */
uint32_t connection_step(connection *c);
void connection_destroy(connection *c);
void connection_io_complete(connection *c, io_job *job);
int connection_waiting(const connection *c);
void connection_drain(connection *c);
#endif
