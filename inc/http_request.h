#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include "http_headers.h"
#include "http_request.h"
#include <span.h>

// https://tools.ietf.org/html/rfc2616#page-100


typedef struct http_request
{
    span_t buffer;
    span_t method;
    span_t path;
    span_t version;
    span_t headers;
} http_request_t;

HL_RESULT http_request_get_buffer(http_request_t* request, span_t* buffer);

HL_RESULT http_request_parse(http_request_t* request, span_t buffer);

HL_RESULT http_request_get_method(http_request_t* request, span_t* method);

HL_RESULT http_request_get_http_version(http_request_t* request, span_t* version);

HL_RESULT http_request_get_path(http_request_t* request, span_t* path);

HL_RESULT http_request_get_headers(http_request_t* request, http_headers_t* headers);

HL_RESULT http_request_read_body(http_request_t* request, span_t* buffer);


#endif // HTTP_REQUEST_H
