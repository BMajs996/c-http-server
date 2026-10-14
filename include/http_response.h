#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H
#include <stddef.h>
#include <stdint.h>
#include "http_parser.h"
#define HTTP_FIELD_MAX 128
struct http_field { unsigned short name, value; };
struct http_fields { char text[HEADER_LIMIT+1]; struct http_field fields[HTTP_FIELD_MAX]; size_t count; };
/* Copy and validate a complete header block. The first line is text[0]. */
int http_fields_parse(struct http_fields *fields,const char *data,size_t length);
const char *http_field_name(const struct http_fields *fields,size_t index);
const char *http_field_value(const struct http_fields *fields,size_t index);
/* Validate Connection options; framing/authority fields cannot be nominated. */
int http_connection_valid(const struct http_fields *fields);
int http_field_hop(const struct http_fields *fields,size_t index);
enum http_framing { HTTP_NONE, HTTP_LENGTH, HTTP_CHUNKED, HTTP_EOF };
struct http_response { struct http_fields headers; int status; enum http_framing framing; uint64_t length; };
int http_response_parse(struct http_response *response,const char *data,size_t length,int head);
enum http_chunk_state { CHUNK_SIZE, CHUNK_DATA, CHUNK_CR, CHUNK_LF, CHUNK_TRAILERS, CHUNK_DONE };
struct http_chunks { uint64_t remaining; enum http_chunk_state state; char line[1024]; size_t used, trailer_bytes, trailer_count; };
/* Incremental decode: 0 ongoing, 1 complete, -1 invalid. Advances input/output
 * counts; never allocates according to a declared chunk size. */
int http_chunks_decode(struct http_chunks *chunks,const char *input,size_t *input_length,char *output,size_t *output_length);
#endif
