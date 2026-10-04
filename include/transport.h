#ifndef TRANSPORT_H
#define TRANSPORT_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
typedef struct transport transport;
int transport_init(void);
void transport_close(void);
int transport_tls_enabled(void);
transport *transport_create(int fd);
void transport_destroy(transport *t);
/* Handshake/shutdown: 1 complete, 0 retry using interest, -1 fatal. */
int transport_handshake(transport *t, uint32_t *interest);
int transport_shutdown(transport *t, uint32_t *interest);
/* Socket I/O: positive bytes, 0 EOF, -1 fatal, -2 wait (or EINTR retry). */
ssize_t transport_read(transport *t, void *data, size_t length, uint32_t *interest);
ssize_t transport_write(transport *t, const void *data, size_t length, uint32_t *interest);
/* Plain HTTP file transfer: positive bytes, 0 premature EOF, -1 fatal,
 * -2 retry using interest (or after EINTR), -3 use buffered I/O instead.
 * A successful partial transfer advances *offset by exactly the bytes sent. */
ssize_t transport_sendfile(transport *t, int file_fd, off_t *offset, size_t length,
                           uint32_t *interest);
uint64_t transport_handshakes(void);
uint64_t transport_failed_handshakes(void);
#endif
