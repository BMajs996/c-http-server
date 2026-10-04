#ifndef LOGGING_H
#define LOGGING_H
#include <stdint.h>
#include <stddef.h>
int logging_init(void);
void logging_close(void);
uint64_t logging_dropped(void);
uint64_t logging_written(void);
size_t logging_queued(void);
void log_request(int fd, unsigned sequence, const char *target, const char *method,
                 int status, uintmax_t bytes, int64_t elapsed_ms, int complete);
#endif
