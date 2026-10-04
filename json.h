#ifndef JSON_H
#define JSON_H
#include <stddef.h>
/* Validate a complete UTF-8 JSON value; nesting is limited to 32 levels. */
int json_valid(const char *data, size_t length);
#endif
