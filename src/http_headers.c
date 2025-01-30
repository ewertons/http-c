#include <stddef.h>

#include "http_headers.h"
#include "common.h"

#include <span.h>
#include "niceties.h"


HL_RESULT http_headers_init(http_headers_t* headers, span_t buffer)
{
    HL_RESULT result;

    if (headers == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        headers->buffer = buffer;
        headers->used_size = 0;
        headers->iterator = SPAN_EMPTY;

        result = HL_RESULT_OK;
    }

    return result;
}

result_t http_headers_parse(http_headers_t* headers, span_t raw_headers)
{
    result_t result;

    if (headers == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        headers->buffer = raw_headers;
        headers->used_size = span_get_size(raw_headers); // TODO: get the size up to crlf crlf or end.
        headers->iterator = raw_headers;

        result = ok;
    }

    return result;
}

HL_RESULT http_headers_get_buffer(http_headers_t* headers, span_t* buffer)
{
    HL_RESULT result;

    if (headers == NULL || buffer == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        *buffer = span_slice(headers->buffer, 0, headers->used_size);;

        result = HL_RESULT_OK;
    }

    return result;
}

HL_RESULT http_headers_get_next(http_headers_t* headers, span_t* name, span_t* value)
{
    HL_RESULT result;

    if (headers == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        if (headers->used_size == 0)
        {
            result = HL_RESULT_EOF;
        }
        else
        {
            if (span_is_empty(headers->iterator) || span_compare(headers->iterator, crlf) == 0)
            {
                headers->iterator = span_slice(headers->buffer, 0, headers->used_size);
                result = HL_RESULT_EOF;
            }
            else
            {
                span_t iterator = headers->iterator;
                span_t current_header;

                if (span_split(headers->iterator, 0, crlf, &current_header, &headers->iterator) != 0)
                {
                    result = HL_RESULT_ERROR;
                }
                else
                {
                    if (span_split(current_header, 0, name_value_separator, name, value) != 0)
                    {
                        result = HL_RESULT_ERROR;
                    }
                    else
                    {
                        result = HL_RESULT_OK;
                    }
                }
            }
        }
    }

    return result;
}

HL_RESULT http_headers_find(http_headers_t* headers, span_t name, span_t* value)
{
    HL_RESULT result;

    if (headers == NULL || span_is_empty(name) || value == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else if (headers->used_size == 0)
    {
        result = HL_RESULT_NOT_FOUND;
    }
    else
    {
        span_t remaining_headers = span_slice(headers->buffer, 0, headers->used_size);
        span_t current_header;

        result = HL_RESULT_NOT_FOUND;

        while(is_success(span_iterate(remaining_headers, crlf, &current_header, &remaining_headers)))
        {
            span_t current_header_name, current_header_value;

            if (span_split(current_header, 0, name_value_separator, &current_header_name, &current_header_value) != 0)
            {
                result = HL_RESULT_ERROR;
            }
            else if (span_compare(current_header_name, name) == 0)
            {
                result = HL_RESULT_OK;
                *value = current_header_value;
                break;
            }
        }
    }

    return result;
}

HL_RESULT http_headers_add(http_headers_t* headers, span_t name, span_t value)
{
    HL_RESULT result;

    if (headers == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        span_t free_buffer = span_slice_to_end(headers->buffer, headers->used_size);

        if (span_is_empty(span_copy(free_buffer, name, &free_buffer)))
        {
            result = HL_RESULT_BUFFER_OVERFLOW;
        }
        else if (span_is_empty(span_copy(free_buffer, name_value_separator, &free_buffer)))
        {
            result = HL_RESULT_BUFFER_OVERFLOW;
        }
        else if (span_is_empty(span_copy(free_buffer, value, &free_buffer)))
        {
            result = HL_RESULT_BUFFER_OVERFLOW;
        }
        else if (span_is_empty(span_copy(free_buffer, crlf, &free_buffer)))
        {
            result = HL_RESULT_BUFFER_OVERFLOW;
        }
        else
        {
            headers->used_size = span_get_size(headers->buffer) - span_get_size(free_buffer);

            result = HL_RESULT_OK;
        }
    }

    return result;
}

result_t http_headers_serialize_to(http_headers_t* headers, stream_t* stream)
{
    result_t result;

    if (headers == NULL || stream == NULL)
    {
        result = invalid_argument;
    }
    else 
    {
        if (headers->used_size == 0)
        {
            result = stream_write(stream, crlf, NULL);
        }
        else 
        {
            result = stream_write(stream, span_slice(headers->buffer, 0, headers->used_size), NULL);
        }
    }

    return result;
}