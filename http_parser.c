#include "http_parser.h"
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

static int token(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c) != NULL;
}
static int connection_tokens(char *value, int *close_requested, int *keep_requested) {
    for (;;) {
        while (*value == ' ' || *value == '\t') ++value;
        char *start = value;
        while (*value && token((unsigned char)*value)) ++value;
        if (value == start) return 400;
        char *end = value;
        while (*value == ' ' || *value == '\t') ++value;
        if (*value && *value != ',') return 400;
        int more = *value == ',';
        char *next = more ? value + 1 : value;
        *end = 0;
        if (!strcasecmp(start, "close")) *close_requested = 1;
        if (!strcasecmp(start, "keep-alive")) *keep_requested = 1;
        if (!more) return 0;
        value = next;
    }
}
/* Combine repeated codings conservatively: their lowest quality wins. */
static int encoding_preferences(char *value,int *gzip,int *identity,int *wildcard) {
    char *p=value;
    while(*p) {
        while(*p==' ' || *p=='\t' || *p==',')++p;
        if(!*p)break;
        char *name=p;
        while(*p && token((unsigned char)*p))++p;
        if(p==name)return 400;
        size_t length=(size_t)(p-name);
        while(*p==' ' || *p=='\t')++p;
        int quality=1000;
        if(*p==';') {
            ++p;while(*p==' ' || *p=='\t')++p;
            if(*p!='q' && *p!='Q')return 400;
            ++p;while(*p==' ' || *p=='\t')++p;
            if(*p++!='=')return 400;
            while(*p==' ' || *p=='\t')++p;
            if(*p!='0' && *p!='1')return 400;
            int whole=*p++-'0',fraction=0,digits=0;
            if(*p=='.') {
                ++p;
                while(*p>='0' && *p<='9') {
                    if(++digits>3)return 400;
                    fraction=fraction*10+(*p++-'0');
                }
            }
            if(whole && fraction)return 400;
            while(digits++<3)fraction*=10;
            quality=whole*1000+fraction;
            while(*p==' ' || *p=='\t')++p;
        }
        if(*p && *p!=',')return 400;
        int *selected=NULL;
        if(length==4 && !strncasecmp(name,"gzip",4))selected=gzip;
        else if(length==8 && !strncasecmp(name,"identity",8))selected=identity;
        else if(length==1 && *name=='*')selected=wildcard;
        if(selected && (*selected<0 || quality<*selected))*selected=quality;
        if(*p)++p;
    }
    return 0;
}
int parse_request(const char *data, size_t length, struct http_request *r) {
    char text[HEADER_LIMIT + 1];
    if (length < 4 || length > HEADER_LIMIT || memchr(data, 0, length)) return 400;
    memcpy(text, data, length); text[length] = 0;
    memset(r, 0, sizeof *r);
    char *end = strstr(text, "\r\n");
    if (!end) return 400;
    *end = 0;
    char *method = text, *target = strchr(method, ' ');
    if (!target) return 400;
    *target++ = 0;
    char *version = strchr(target, ' ');
    if (!version) return 400;
    *version++ = 0;
    if (!*method || !*target || strchr(version, ' ')) return 400;
    for (const unsigned char *p = (unsigned char *)method; *p; ++p)
        if (!token(*p)) return 400;
    if (strlen(method) >= sizeof r->method) return 400;
    strcpy(r->method, method);
    if (target[0] != '/') return 400;
    for (const unsigned char *p = (unsigned char *)target; *p; ++p)
        if (*p <= 32 || *p >= 127 || *p == '#') return 400;
    if (strlen(target) >= sizeof r->target) return 414;
    strcpy(r->target, target);
    r->head = !strcmp(method, "HEAD");
    r->http11 = !strcmp(version, "HTTP/1.1");
    if (!r->http11 && strcmp(version, "HTTP/1.0")) return 505;
    int hosts = 0, lengths = 0, close_requested = 0, keep_requested = 0;
    int transfer = 0, expect = 0;
    int gzip=-1,identity=-1,wildcard=-1;
    uint64_t content_length = 0;
    char *line = end + 2;
    while (*line) {
        end = strstr(line, "\r\n");
        if (!end) return 400;
        if (end == line) break;
        *end = 0;
        char *colon = strchr(line, ':');
        if (!colon || colon == line) return 400;
        for (char *p = line; p < colon; ++p) if (!token((unsigned char)*p)) return 400;
        *colon = 0;
        char *value = colon + 1;
        for (const unsigned char *p = (unsigned char *)value; *p; ++p)
            if ((*p < 32 && *p != '\t') || *p == 127) return 400;
        while (*value == ' ' || *value == '\t') ++value;
        char *tail = value + strlen(value);
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        if (!strcasecmp(line, "Host")) {
            if (++hosts > 1 || !*value) return 400;
            /* Deliberately conservative authority syntax: DNS/IPv4 and
             * bracketed IPv6, with an optional decimal port. */
            const char *p = value;
            if (*p == '[') {
                ++p; int count = 0;
                while (*p && *p != ']') {
                    if (!isxdigit((unsigned char)*p) && *p != ':' && *p != '.') return 400;
                    ++count; ++p;
                }
                if (!count || *p++ != ']') return 400;
            } else {
                int count = 0;
                while (*p && *p != ':') {
                    if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-') return 400;
                    ++count; ++p;
                }
                if (!count) return 400;
            }
            if (*p == ':') {
                ++p; if (!*p) return 400;
                unsigned port = 0;
                while (*p) {
                    if (*p < '0' || *p > '9') return 400;
                    port = port * 10 + (unsigned)(*p++ - '0');
                    if (port > 65535) return 400;
                }
            }
            if (*p) return 400;
        } else if (!strcasecmp(line, "Content-Length")) {
            if (++lengths > 1 || !*value) return 400;
            for (const unsigned char *p = (unsigned char *)value; *p; ++p) {
                if (*p < '0' || *p > '9' || content_length > (UINT64_MAX - (*p - '0')) / 10) return 400;
                content_length = content_length * 10 + (*p - '0');
            }
        } else if (!strcasecmp(line, "Content-Type") || !strcasecmp(line, "Content-Encoding")) {
            int type = !strcasecmp(line, "Content-Type");
            char *dest = type ? r->content_type : r->content_encoding;
            size_t capacity = type ? sizeof r->content_type : sizeof r->content_encoding;
            if (*dest || !*value) return 400;
            if (strlen(value) >= capacity) return 431;
            strcpy(dest,value);
        } else if (!strcasecmp(line, "If-None-Match")) {
            size_t old = strlen(r->if_none_match), add = strlen(value);
            if (!add || old + add + 2 >= sizeof r->if_none_match) return 431;
            if (old) strcat(r->if_none_match, ",");
            strcat(r->if_none_match, value);
        } else if (!strcasecmp(line, "Accept-Encoding")) {
            if(encoding_preferences(value,&gzip,&identity,&wildcard))return 400;
        } else if (!strcasecmp(line, "Range")) {
            if (*r->range || !*value) return 400;
            if (strlen(value) >= sizeof r->range) return 431;
            strcpy(r->range, value);
        } else if (!strcasecmp(line, "If-Range")) {
            if (*r->if_range || !*value) return 400;
            if (strlen(value) >= sizeof r->if_range) return 431;
            strcpy(r->if_range, value);
        } else if (!strcasecmp(line, "Transfer-Encoding")) transfer = 1;
        else if (!strcasecmp(line, "Expect")) expect = 1;
        else if (!strcasecmp(line, "Connection")) {
            if (connection_tokens(value, &close_requested, &keep_requested)) return 400;
        }
        line = end + 2;
    }
    if (r->http11 && hosts != 1) return 400;
    if (transfer && lengths) return 400;
    /* Framing errors close the connection; unread bytes cannot be reused. */
    if (transfer) return 501;
    if (expect) return 417;
    if (content_length > BODY_LIMIT) return 413;
    r->content_length = (size_t)content_length;
    r->gzip_q=gzip>=0?gzip:wildcard>=0?wildcard:0;
    r->identity_q=identity>=0?identity:wildcard==0?0:1000;
    r->keep_alive = !close_requested && (r->http11 || keep_requested);
    return 0;
}
