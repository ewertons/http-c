#include <stddef.h>

#include <span.h>
#include "niceties.h"

#include <http_request.h>
#include "common.h"

static result_t parse_request(http_request_t *request)
{
    result_t result;

    span_t raw_request = request->buffer;

    if (span_split(raw_request, 0, space, &request->method, &raw_request) != 0)
    {
        result = error;
    }
    else if (span_split(raw_request, 0, space, &request->path, &raw_request) != 0)
    {
        result = error;
    }
    else if (span_split(raw_request, 0, crlf, &request->version, &raw_request) != 0)
    {
        result = error;
    }
    else
    {
        request->version = span_slice_to_end(request->version, 5 /* sizeof("HTTP/") */);
        request->headers = raw_request;
        result = ok;
    }

    return result;
}

result_t http_request_get_buffer(http_request_t *request, span_t* buffer)
{
    result_t result;

    if (request == NULL || buffer == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        *buffer = request->buffer;
        result = ok;
    }

    return result;
}

result_t http_request_parse(http_request_t *request, span_t buffer)
{
    result_t result;

    if (request == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        request->buffer = buffer;

        result = parse_request(request);
    }

    return result;
}

result_t http_request_get_method(http_request_t *request, span_t* method)
{
    result_t result;

    if (request == NULL || method == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        *method = request->method;
        result = ok;
    }

    return result;
}

result_t http_request_get_http_version(http_request_t *request, span_t* version)
{
    result_t result;

    if (request == NULL || version == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        *version = request->version;
        result = ok;
    }

    return result;
}

result_t http_request_get_path(http_request_t *request, span_t* path)
{
    result_t result;

    if (request == NULL || path == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        *path = request->path;
        result = ok;
    }

    return result;
}

result_t http_request_get_headers(http_request_t *request, http_headers_t *headers)
{
    result_t result;

    if (request == NULL || headers == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        result = http_headers_parse(headers, request->headers);
    }

    return result;
}

result_t http_request_read_body(http_request_t *request, span_t* buffer)
{
    result_t result;

    if (request == NULL || buffer == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        result = error;
    }

    return result;   
}

result_t http_request_serialize_to(http_request_t* request, stream_t* stream)
{
    result_t result;

    if (request == NULL || stream == NULL)
    {
        result = invalid_argument;
    }
    else
    {

        result = ok;
    }

    return result;
}