#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include "http_headers.h"

#include "span.h" 
#include "niceties.h" 

// https://tools.ietf.org/html/rfc2616#page-100


typedef struct http_response 
{
    span_t buffer;
    uint32_t used_size;
    http_headers_t* headers;
    span_t body;
} http_response_t;


result_t http_response_init(http_response_t* response, span_t buffer, span_t http_version, span_t code, span_t reason_phrase);

result_t http_response_get_code(http_response_t response, span_t* code);

result_t http_response_get_reason_phrase(http_response_t response, span_t* reason_phrase);

result_t http_response_get_http_version(http_response_t response, span_t* http_version);

result_t http_response_set_headers(http_response_t* response, http_headers_t* headers);

result_t http_response_get_headers(http_response_t response, http_headers_t** headers);

result_t http_response_set_body(http_response_t* response, span_t body);

#endif // HTTP_RESPONSE_H
