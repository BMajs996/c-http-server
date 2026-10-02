#ifndef CACHE_H
#define CACHE_H
#include <stdint.h>
int etag_matches(const char *condition, const char *etag);
/* 1 valid range; 0 ignore malformed/multiple; -1 unsatisfiable. */
int byte_range(const char *value, uintmax_t size, uintmax_t *start, uintmax_t *length);
#endif
