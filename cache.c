#include "cache.h"
#include <string.h>
static int decimal(const char **p, uintmax_t *n) {
    *n = 0;
    if (**p < '0' || **p > '9') return 0;
    while (**p >= '0' && **p <= '9') {
        unsigned digit = (unsigned)(*(*p)++ - '0');
        if (*n > (UINTMAX_MAX - digit) / 10) return 0;
        *n = *n * 10 + digit;
    }
    return 1;
}
int etag_matches(const char *condition, const char *etag) {
    if (!strncmp(etag, "W/", 2)) etag += 2;
    const char *p = condition;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        if (*p == '*') {
            ++p; while (*p == ' ' || *p == '\t') ++p;
            return !*p;
        }
        if (!strncmp(p, "W/", 2)) p += 2;
        if (*p != '"') return 0;
        const char *start = p++;
        while (*p && *p != '"') ++p;
        if (!*p) return 0;
        ++p;
        size_t size = (size_t)(p - start);
        int matched = strlen(etag) == size && !memcmp(etag, start, size);
        while (*p == ' ' || *p == '\t') ++p;
        if (*p && *p != ',') return 0;
        if (matched) return 1;
    }
    return 0;
}
int byte_range(const char *value, uintmax_t size, uintmax_t *start, uintmax_t *length) {
    if (strncmp(value, "bytes=", 6) || strchr(value, ',')) return 0;
    const char *p = value + 6;
    uintmax_t first, last;
    if (*p == '-') {
        ++p;
        if (!decimal(&p, &last) || *p) return 0;
        if (!last || !size) return -1;
        *length = last < size ? last : size;
        *start = size - *length;
        return 1;
    }
    if (!decimal(&p, &first) || *p++ != '-') return 0;
    if (*p) {
        if (!decimal(&p, &last) || *p || last < first) return 0;
    } else last = size ? size - 1 : 0;
    if (first >= size) return -1;
    if (last >= size) last = size - 1;
    *start = first; *length = last - first + 1;
    return 1;
}
