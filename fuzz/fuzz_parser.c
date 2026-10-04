/* Dependency-free deterministic mutation runner; not coverage-guided fuzzing.
 * Also exposes a libFuzzer entry point for Clang when available. */
#include "http_parser.h"
#include "cache.h"
#include "json.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct http_request a, b;
    int first = parse_request((const char *)data, size, &a);
    int second = parse_request((const char *)data, size, &b);
    assert(first == second);
    if (!first) {
        assert(a.target[0] == '/' && !strcmp(a.target, b.target));
        assert(a.method[0] && a.keep_alive == b.keep_alive);
        assert(a.content_length<=BODY_LIMIT && a.content_length==b.content_length);
        assert(a.head == 0 || a.head == 1);
        assert(a.gzip_q>=0 && a.gzip_q<=1000 && a.identity_q>=0 && a.identity_q<=1000);
        assert(a.gzip_q==b.gzip_q && a.identity_q==b.identity_q);
    }
    int valid_json=json_valid((const char *)data,size);
    assert(valid_json==0 || valid_json==1);
    assert(valid_json==json_valid((const char *)data,size));
    char value[256];
    size_t length = size < sizeof value - 1 ? size : sizeof value - 1;
    memcpy(value, data, length); value[length] = 0;
    uintmax_t start, count;
    int result = byte_range(value, 4096, &start, &count);
    if (result > 0) assert(count > 0 && start < 4096 && count <= 4096 - start);
    (void)etag_matches(value, "W/\"example\"");
    return 0;
}
#ifndef LIBFUZZER
static uint32_t random_state = 0x12345678;
static uint32_t next_random(void) {
    random_state ^= random_state << 13; random_state ^= random_state >> 17;
    random_state ^= random_state << 5; return random_state;
}
int main(int argc, char **argv) {
    unsigned long iterations = 100000;
    if (argc == 2) {
        char *end; iterations = strtoul(argv[1], &end, 10);
        if (*end || !iterations || iterations > 10000000) return 2;
    } else if (argc > 2) return 2;
    const char *seeds[] = {
        "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "HEAD /health HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nRange: bytes=0-10\r\nIf-None-Match: W/\"abc\"\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: [::1]:80\r\nTransfer-Encoding: chunked\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nAccept-Encoding: gzip;q=1, identity;q=0.5\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: a\r\nAccept-Encoding: *;q=0\r\nAccept-Encoding: gzip;q=0.7\r\n\r\n",
        "{\"nested\":[true,null,1.2e3,\"text\"]}",
        "POST /api/echo HTTP/1.1\r\nHost: a\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n",
        "bytes=0-4095", "bytes=-20", "bytes=18446744073709551615-"
    };
    uint8_t data[HEADER_LIMIT + 32];
    for (unsigned long i = 0; i < iterations; ++i) {
        const char *seed = seeds[next_random() % (sizeof seeds / sizeof seeds[0])];
        size_t length = strlen(seed); memcpy(data, seed, length);
        if (i % 4 == 0) {
            length = next_random() % sizeof data;
            for (size_t j = 0; j < length; ++j) data[j] = (uint8_t)next_random();
        } else {
            unsigned edits = 1 + next_random() % 12;
            for (unsigned j = 0; j < edits && length; ++j) {
                size_t at = next_random() % length;
                switch (next_random() % 3) {
                case 0: data[at] = (uint8_t)next_random(); break;
                case 1: memmove(data + at, data + at + 1, length - at - 1); --length; break;
                default:
                    if (length < sizeof data) {
                        memmove(data + at + 1, data + at, length - at);
                        data[at] = (uint8_t)next_random(); ++length;
                    }
                }
            }
        }
        LLVMFuzzerTestOneInput(data, length);
    }
    printf("Passed %lu deterministic parser/range/JSON mutations (seed 0x12345678).\n", iterations);
    return 0;
}
#endif
