#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include "http_headers.h"
#include "http_request.h"
#include "stream.h"
#include <span.h>

// https://tools.ietf.org/html/rfc2616#page-100


typedef struct http_request
{
    span_t method;
    span_t path;
    span_t http_version;
    http_headers_t headers;
} http_request_t;

result_t http_request_initialize(http_request_t *request, span_t method, span_t path, span_t http_version, http_headers_t headers);

result_t http_request_parse(http_request_t* request, span_t raw_request);

static inline result_t http_request_get_method(http_request_t* request, span_t* method)
{
    if (request == NULL || method == NULL)
    {
        return invalid_argument;
    }
    else
    {
        *method = request->method;
        return ok;
    }
}

static inline result_t http_request_get_http_version(http_request_t* request, span_t* http_version)
{
    if (request == NULL || http_version == NULL)
    {
        return invalid_argument;
    }
    else
    {
        *http_version = request->http_version;
        return ok;
    }
}

static inline result_t http_request_get_path(http_request_t* request, span_t* path)
{
    if (request == NULL || path == NULL)
    {
        return invalid_argument;
    }
    else
    {
        *path = request->path;
        return ok;
    }
}

static inline result_t http_request_get_headers(http_request_t* request, http_headers_t* headers)
{
    if (request == NULL || headers == NULL)
    {
        return invalid_argument;
    }
    else
    {
        *headers = request->headers;
        return ok;
    }
}

result_t http_request_serialize_to(http_request_t* request, stream_t* stream);

#endif // HTTP_REQUEST_H
