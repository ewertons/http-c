#include <stddef.h>
#include <http_response.h>
#include <span.h>

result_t http_response_init(http_response_t* response, span_t buffer, span_t http_version, span_t code, span_t reason_phrase)
{
    result_t result;

    if (response == NULL || span_is_empty(buffer) || span_is_empty(http_version) || span_is_empty(code) || span_is_empty(reason_phrase))
    {
        result = invalid_argument;
    }
    else
    {
        response->buffer = buffer;

        if (span_is_empty(span_copy(buffer, http_version, &buffer)))
        {
            result = insufficient_size;
        }
        else if (span_is_empty(span_copy(buffer, space, &buffer)))
        {
            result = insufficient_size;
        }
        else if (span_is_empty(span_copy(buffer, code, &buffer)))
        {
            result = insufficient_size;
        }
        else if (span_is_empty(span_copy(buffer, space, &buffer)))
        {
            result = insufficient_size;
        }
        else if (span_is_empty(span_copy(buffer, reason_phrase, &buffer)))
        {
            result = insufficient_size;
        }
        else if (span_is_empty(span_copy(buffer, crlf, &buffer)))
        {
            result = insufficient_size;
        }
        else
        {
            response->used_size = span_get_size(response->buffer) - span_get_size(buffer);
            response->headers = NULL;
            response->body = SPAN_EMPTY;
            result = ok;
        }
    }

    return result;
}

result_t http_response_get_code(http_response_t response, span_t* code)
{
    result_t result;

    if (response.used_size == 0 || span_get_size(response.buffer) == 0 || code == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        span_t buffer = span_slice(response.buffer, 0, response.used_size);
        span_t data;

        if(is_error(span_iterate(buffer, space, &data, &buffer)))
        {
            result = not_found;
        }
        else if (span_is_empty(data))
        {
            result = not_found;
        }
        else if(is_error(span_iterate(buffer, space, &data, &buffer)))
        {
            result = not_found;
        }
        else if (span_is_empty(data))
        {
            result = not_found;
        }
        else
        {
            *code = data;
            result = ok;
        }
    }

    return result;
}

result_t http_response_get_reason_phrase(http_response_t response, span_t* reason_phrase)
{
    result_t result;

    if (response.used_size == 0 || span_get_size(response.buffer) == 0 || reason_phrase == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        span_t buffer = span_slice(response.buffer, 0, response.used_size);
        span_t data;

        if(is_error(span_iterate(buffer, space, &data, &buffer)))
        {
            result = not_found;
        }
        else if (span_is_empty(data))
        {
            result = not_found;
        }
        else if(is_error(span_iterate(buffer, space, &data, &buffer)))
        {
            result = not_found;
        }
        else if (span_is_empty(data))
        {
            result = not_found;
        }
        else if(is_error(span_iterate(buffer, crlf, &data, &buffer)))
        {
            result = not_found;
        }
        else if (span_is_empty(data))
        {
            result = not_found;
        }
        else
        {
            *reason_phrase = data;
            result = ok;
        }
    }

    return result;
}

result_t http_response_get_http_version(http_response_t response, span_t* http_version)
{
    result_t result;

    if (response.used_size == 0 || span_get_size(response.buffer) == 0 || http_version == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        span_t buffer = span_slice(response.buffer, 0, response.used_size);
        span_t data;

        if(is_error(span_iterate(buffer, space, &data, &buffer)))
        {
            result = not_found;
        }
        else if (span_is_empty(data))
        {
            result = not_found;
        }
        else
        {
            *http_version = data;
            result = ok;
        }
    }

    return result;
}

result_t http_response_set_headers(http_response_t* response, http_headers_t* headers)
{
    HL_RESULT result;

    if (response == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        response->headers = headers;

        result = HL_RESULT_OK;
    }

    return result;
}

result_t http_response_get_headers(http_response_t response, http_headers_t** headers)
{
    HL_RESULT result;

    if (headers == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        *headers = response.headers;

        result = HL_RESULT_OK;
    }

    return result;
}

result_t http_response_set_body(http_response_t* response, span_t body)
{
    // uint8_t raw_buffer[64];
    // buffer buffer = buffer_from_array(raw_buffer);

    // body->open(body);
    // int l = body->get_data(buffer, body);
    // body->close(body);

    // send(raw_buffer, l)

    HL_RESULT result;

    if (response == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        response->body = body;

        result = HL_RESULT_OK;
    }

    return result;
}
