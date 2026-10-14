#ifndef HTTP_SYNTAX_H
#define HTTP_SYNTAX_H
#include <string.h>
/* Preserve the last three bytes between reads; fragmented headers are scanned
 * once instead of rescanning their whole prefix after each socket read. */
static inline size_t http_header_end(const char *data,size_t length,size_t *scan) {
    for(size_t i=*scan;i+3<length;i++) {
        if(!memcmp(data+i,"\r\n\r\n",4)){*scan=i;return i+4;}
    }
    *scan=length>3?length-3:0;
    return 0;
}
/* Shared field grammar; request/response semantics stay in their parsers. */
static inline int http_token(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c));
}
static inline int http_split_field(char *line, char **value) {
    char *colon = strchr(line, ':');
    if (!colon || colon == line) return -1;
    for (char *p = line; p < colon; ++p)
        if (!http_token((unsigned char)*p)) return -1;
    *colon = 0;
    *value = colon + 1;
    for (const unsigned char *p = (unsigned char *)*value; *p; ++p)
        if ((*p < 32 && *p != '\t') || *p == 127) return -1;
    while (**value == ' ' || **value == '\t') ++*value;
    char *end = *value + strlen(*value);
    while (end > *value && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    return 0;
}
#endif
