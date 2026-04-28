#include <stddef.h>
#include <string.h>

#include <span.h>
#include "niceties.h"
#include "stream.h"

#include <http_request.h>
#include "common.h"

result_t http_request_initialize(http_request_t *request, span_t method, span_t path, span_t http_version, http_headers_t headers)
{
    result_t result;

    if (request == NULL || span_is_empty(method) || span_is_empty(path) || span_is_empty(http_version))
    {
        result = invalid_argument;
    }
    else
    {
        (void)memset(request, 0, sizeof(http_request_t));
        request->method = method;
        request->path = path;
        request->http_version = http_version;
        request->headers = headers;
        request->body = SPAN_EMPTY;

        result = ok;
    }

    return result;
}

result_t http_request_parse(http_request_t *request, span_t raw_request)
{
    result_t result;

    if (request == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        request->body = SPAN_EMPTY;

        if (span_split(raw_request, 0, space, &request->method, &raw_request) != 0)
        {
            result = error;
        }
        else if (span_split(raw_request, 0, space, &request->path, &raw_request) != 0)
        {
            result = error;
        }
        else if (span_split(raw_request, 0, crlf, &request->http_version, &raw_request) != 0)
        {
            result = error;
        }
        else
        {
            /* Split headers and body at the CRLFCRLF boundary if present. */
            span_t headers_part = raw_request;
            span_t body_part = SPAN_EMPTY;
            int term = span_find(raw_request, 0, headers_terminator, &body_part);
            if (term != -1)
            {
                headers_part = span_slice(raw_request, 0, (uint32_t)term + (uint32_t)span_get_size(crlf));
                request->body = body_part;
            }

            result = http_headers_parse(&request->headers, headers_part);
        }
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
        else if (failed(stream_write(stream, request->http_version, NULL)))
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
        /* End of header block. */
        else if (failed(stream_write(stream, crlf, NULL)))
        {
            result = error;
        }
        else if (!span_is_empty(request->body) &&
                 failed(stream_write(stream, request->body, NULL)))
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
