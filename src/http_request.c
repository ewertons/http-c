#include <stddef.h>

#include <span.h>
#include "niceties.h"

#include <http_request.h>
#include "common.h"

static int parse_request(http_request_t *request)
{
    int result;

    span_t raw_request = request->buffer;

    if (span_split(raw_request, 0, space, &request->method, &raw_request) != 0)
    {
        result = ERROR;
    }
    else if (span_split(raw_request, 0, space, &request->path, &raw_request) != 0)
    {
        result = ERROR;
    }
    else if (span_split(raw_request, 0, crlf, &request->version, &raw_request) != 0)
    {
        result = ERROR;
    }
    else
    {
        request->version = span_slice_to_end(request->version, 5 /* sizeof("HTTP/") */);
        request->headers = raw_request;
        result = OK;
    }

    return result;
}

HL_RESULT http_request_get_buffer(http_request_t *request, span_t* buffer)
{
    HL_RESULT result;

    if (request == NULL || buffer == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        *buffer = request->buffer;
        result = HL_RESULT_OK;
    }

    return result;
}

HL_RESULT http_request_parse(http_request_t *request, span_t buffer)
{
    HL_RESULT result;

    if (request == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        request->buffer = buffer;

        if (parse_request(request) != OK)
        {
            result = HL_RESULT_ERROR;
        }
        else
        {
            result = HL_RESULT_OK;
        }
    }

    return result;
}

HL_RESULT http_request_get_method(http_request_t *request, span_t* method)
{
    HL_RESULT result;

    if (request == NULL || method == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        *method = request->method;
        result = HL_RESULT_OK;
    }

    return result;
}

HL_RESULT http_request_get_http_version(http_request_t *request, span_t* version)
{
    HL_RESULT result;

    if (request == NULL || version == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        *version = request->version;
        result = HL_RESULT_OK;
    }

    return result;
}

HL_RESULT http_request_get_path(http_request_t *request, span_t* path)
{
    HL_RESULT result;

    if (request == NULL || path == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        *path = request->path;
        result = HL_RESULT_OK;
    }

    return result;
}

HL_RESULT http_request_get_headers(http_request_t *request, http_headers_t *headers)
{
    HL_RESULT result;

    if (request == NULL || headers == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        if (http_headers_parse(headers, request->headers) != HL_RESULT_OK)
        {
            result = HL_RESULT_ERROR;
        }
        else
        {
            result = HL_RESULT_OK;
        }
    }

    return result;
}

HL_RESULT http_request_read_body(http_request_t *request, span_t* buffer)
{
    HL_RESULT result;

    if (request == NULL || buffer == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        result = HL_RESULT_ERROR;
    }

    return result;   
}