#include <stddef.h>

#include <span.h>

#include <http_response.h>

result_t http_response_initialize(http_response_t* response, span_t http_version, span_t code, span_t reason_phrase, http_headers_t headers)
{
    result_t result;

    if (response == NULL || span_is_empty(http_version) || span_is_empty(code) || span_is_empty(reason_phrase))
    {
        result = invalid_argument;
    }
    else
    {
        response->http_version = http_version;
        response->code = code;
        response->reason_phrase = reason_phrase;
        response->headers = headers;
        response->body = SPAN_EMPTY;
        result = ok;
    }

    return result;
}

result_t http_response_parse(http_response_t* response, span_t raw_response, span_t* out_raw_response_remainder)
{
    result_t result;

    if (response == NULL || span_is_empty(raw_response))
    {
        result = invalid_argument;
    }
    else
    {
        response->body = SPAN_EMPTY;

        if (span_split(raw_response, 0, space, &response->http_version, &raw_response) != 0)
        {
            result = error;
        }
        else if (span_split(raw_response, 0, space, &response->code, &raw_response) != 0)
        {
            result = error;
        }
        else if (span_split(raw_response, 0, crlf, &response->reason_phrase, &raw_response) != 0)
        {
            result = error;
        }
        else
        {
            span_t headers_part = raw_response;
            span_t body_part = SPAN_EMPTY;
            int term = span_find(raw_response, 0, headers_terminator, &body_part);
            if (term != -1)
            {
                headers_part = span_slice(raw_response, 0, (uint32_t)term + (uint32_t)span_get_size(crlf));
                response->body = body_part;
            }

            result = http_headers_parse(&response->headers, headers_part);
        }

        if (out_raw_response_remainder != NULL)
        {
            *out_raw_response_remainder = SPAN_EMPTY;
        }
    }

    return result;
}

result_t http_response_serialize_to(http_response_t* response, stream_t* stream)
{
    result_t result;

    if (response == NULL || stream == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        if (failed(stream_write(stream, response->http_version, NULL)))
        {
            result = error;
        }
        else if (failed(stream_write(stream, space, NULL)))
        {
            result = error;
        }
        else if (failed(stream_write(stream, response->code, NULL)))
        {
            result = error;
        }
        else if (failed(stream_write(stream, space, NULL)))
        {
            result = error;
        }
        else if (failed(stream_write(stream, response->reason_phrase, NULL)))
        {
            result = error;
        }
        else if (failed(stream_write(stream, crlf, NULL)))
        {
            result = error;
        }
        else if (failed(http_headers_serialize_to(&response->headers, stream)))
        {
            result = error;
        }
        /* End of header block. */
        else if (failed(stream_write(stream, crlf, NULL)))
        {
            result = error;
        }
        else if (!span_is_empty(response->body) &&
                 failed(stream_write(stream, response->body, NULL)))
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
