#include <stddef.h>

#include "http_headers.h"
#include "common.h"

#include <span.h>
#include "niceties.h"

/* Headers are *written* as "Name: value" but must be *read* far more
 * leniently. RFC 7230 3.2 puts the colon immediately after the name and
 * makes the whitespace around the value optional, so "Content-Length:42"
 * and "Content-Length:  42  " are both legal and both have to parse. */
static const span_t header_name_terminator = span_from_str_literal(":");

/* RFC 7230 3.2.3: optional whitespace either side of a field value is not
 * part of the value. Spaces and horizontal tabs only -- a CR or LF here
 * would mean the header block was framed wrongly, which is not something
 * to paper over. */
static span_t trim_optional_whitespace(span_t value)
{
    uint32_t size = span_get_size(value);

    if (value.ptr == NULL || size == 0)
    {
        return value;
    }

    uint32_t start = 0;
    uint32_t end   = size;

    while (start < end && (value.ptr[start] == ' ' || value.ptr[start] == '\t'))
    {
        start++;
    }
    while (end > start && (value.ptr[end - 1] == ' ' || value.ptr[end - 1] == '\t'))
    {
        end--;
    }

    return span_slice(value, start, end - start);
}


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
        /* Trim at the headers terminator (CRLF CRLF) so the body, if any,
         * is not treated as part of the header block. If the terminator is
         * absent, fall back to the full buffer (caller must have ensured
         * the buffer ends at the boundary already). */
        int term_pos = span_find_reverse(raw_headers, -1, headers_terminator);
        uint32_t headers_size;

        if (term_pos == -1)
        {
            /* Try CRLF at the end. */
            int single = span_find_reverse(raw_headers, -1, crlf);
            if (single == -1)
            {
                headers_size = span_get_size(raw_headers);
            }
            else
            {
                headers_size = (uint32_t)single + (uint32_t)span_get_size(crlf);
            }
        }
        else
        {
            /* Include the trailing CRLF that closes the last header line,
             * but exclude the second CRLF that ends the header block. */
            headers_size = (uint32_t)term_pos + (uint32_t)span_get_size(crlf);
        }

        headers->buffer = raw_headers;
        headers->used_size = headers_size;
        headers->iterator = span_slice(raw_headers, 0, headers_size);

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

        while(span_iterate(remaining_headers, crlf, &current_header, &remaining_headers) == ok)
        {
            span_t current_header_name, current_header_value;

            if (span_split(current_header, 0, header_name_terminator, &current_header_name, &current_header_value) != 0)
            {
                /* No colon at all, so this is not a header. Skipping it
                 * rather than failing the lookup keeps one malformed line
                 * from hiding every header after it. */
                continue;
            }
            /* Field names are case-insensitive (RFC 7230 3.2). Comparing
             * them byte-exact is why a client sending "content-length"
             * had its body silently discarded: the length was never
             * found, so the parser waited for zero body bytes and handed
             * the handler an empty span. */
            else if (span_icompare(current_header_name, name, true) == 0)
            {
                result = HL_RESULT_OK;
                *value = trim_optional_whitespace(current_header_value);
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
            /* Nothing, not a CRLF. Both callers -- the request and the
             * response serialisers -- write the blank line that ends the
             * header block themselves, so emitting one here as well put a
             * stray CRLF into the body of every header-less message. */
            result = ok;
        }
        else 
        {
            result = stream_write(stream, span_slice(headers->buffer, 0, headers->used_size), NULL);
        }
    }

    return result;
}