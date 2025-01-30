#include <stddef.h>
#include <string.h>

#include <span.h>
#include "niceties.h"
#include "stream.h"

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
        // request->version = span_slice_to_end(request->version, 5 /* sizeof("HTTP/") */);
        result = http_headers_parse(&request->headers, raw_request);
    }

    return result;
}

result_t http_request_initialize(http_request_t *request, span_t method, span_t path, span_t version, http_headers_t headers)
{
    result_t result;

    if (request == NULL || span_is_empty(method) || span_is_empty(path) || span_is_empty(version))
    {
        result = invalid_argument;
    }
    else
    {
        (void)memset(request, 0, sizeof(http_request_t));
        request->method = method;
        request->path = path;
        request->version = version;
        request->headers = headers;

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

// TODO: return remainder of buffer that has not been parsed.
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
        *headers = request->headers;
        result = ok;
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
        span_t raw_headers;

        if (failed(stream_write(stream, request->method, NULL)))
        {
            result = error;
        }
        else if (failed(stream_write(stream, space, NULL)))
        {
            result = error;
        }
        else if (failed(stream_write(stream, request->path, NULL)))
        {
            result = error;
        }
        else if (failed(stream_write(stream, space, NULL)))
        {
            result = error;
        }
        else if (failed(stream_write(stream, request->version, NULL)))
        {
            result = error;
        }
        else if (failed(stream_write(stream, crlf, NULL)))
        {
            result = error;
        }
        else if (failed(http_headers_serialize_to(&request->headers, stream)))
        {
            result = error;
        }
        else if (failed(stream_write(stream, crlf, NULL)))
        {
            result = error;
        }
        else if (failed(stream_write(stream, crlf, NULL)))
        {
            result = error;
        }
        else
        {
            result = ok;
        }
    }

    return result;
}
