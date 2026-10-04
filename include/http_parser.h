#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H
#include <stddef.h>
#define HEADER_LIMIT 8192
#define BODY_LIMIT 16384
struct http_request { char method[32]; char target[2048]; int head, keep_alive, http11; int gzip_q, identity_q; size_t content_length; char content_type[128], content_encoding[64]; char if_none_match[1024], range[256], if_range[256]; };
/* Parse one complete header block; 0 means valid, otherwise HTTP error. */
int parse_request(const char *data, size_t length, struct http_request *request);
#endif
