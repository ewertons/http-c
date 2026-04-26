#include <stdlib.h>

#include "span.h"
#include "niceties.h"
#include "socket_stream.h"
#include "task.h"

#include "http_endpoint.h"
#include "http_connection.h"
#include "http_headers.h"

result_t http_connection_set_endpoint(http_connection_t* connection, http_endpoint_t* endpoint)
{
    result_t result;

    if (connection == NULL || endpoint == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        connection->endpoint = endpoint;

        result = ok;
    }

    return result;
}

result_t http_connection_get_endpoint(http_connection_t* connection, http_endpoint_t** endpoint)
{
    result_t result;

    if (connection == NULL || endpoint == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        *endpoint = connection->endpoint;

        result = ok;
    }

    return result;
}

/* Read until the headers terminator (CRLFCRLF) is found, or the buffer is
 * full, or the stream returns an error. Retries on `try_again`. */
static result_t read_until_headers_complete(http_connection_t* connection,
                                            span_t buffer,
                                            span_t* out_received,
                                            span_t* out_remainder)
{
    span_t original_buffer = buffer;
    span_t bytes_read;
    result_t result;

    for (;;)
    {
        if (span_is_empty(buffer))
        {
            return insufficient_size;
        }

        result = stream_read(&connection->stream, buffer, &bytes_read, &buffer);

        if (result == try_again)
        {
            /* Cooperative retry; let the OS schedule. */
            task_sleep_ms(1);
            continue;
        }

        if (is_error(result) || result == end_of_data || result == end_of_file)
        {
            return result;
        }

        span_t received = span_slice_out(original_buffer, buffer);

        if (span_find_reverse(received, -1, headers_terminator) != -1)
        {
            *out_received  = received;
            *out_remainder = buffer;
            return ok;
        }
    }
}

/* Reads `needed` more bytes from the stream into the area immediately after
 * `body_so_far`. Updates `body_so_far` to span the full body. */
static result_t read_remaining_body(http_connection_t* connection,
                                    span_t* body_so_far,
                                    span_t free_buffer,
                                    uint32_t needed)
{
    while (span_get_size(*body_so_far) < needed)
    {
        if (span_is_empty(free_buffer))
        {
            return insufficient_size;
        }

        span_t got;
        result_t r = stream_read(&connection->stream, free_buffer, &got, &free_buffer);

        if (r == try_again)
        {
            task_sleep_ms(1);
            continue;
        }

        if (is_error(r) || r == end_of_data || r == end_of_file)
        {
            return r;
        }

        /* If the body was empty so far, anchor it at the start of the bytes
         * just read; otherwise it's already pointing into the buffer and the
         * fresh bytes are contiguous with it. */
        if (span_is_empty(*body_so_far))
        {
            *body_so_far = got;
        }
        else
        {
            body_so_far->length += span_get_size(got);
        }
    }
    return ok;
}

static result_t maybe_read_body(http_connection_t* connection,
                                http_headers_t* headers,
                                span_t* body,
                                span_t free_buffer)
{
    static const span_t HDR_CONTENT_LENGTH = span_from_str_literal("Content-Length");

    span_t value;
    if (http_headers_find(headers, HDR_CONTENT_LENGTH, &value) != HL_RESULT_OK)
    {
        return ok;
    }

    uint32_t content_length = 0;
    if (span_to_uint32_t(value, &content_length) != 0)
    {
        return error;
    }

    if (content_length == 0)
    {
        return ok;
    }

    return read_remaining_body(connection, body, free_buffer, content_length);
}

result_t http_connection_receive_request(http_connection_t* connection, span_t buffer, http_request_t* request, span_t* out_buffer_remainder)
{
    if (connection == NULL || request == NULL)
    {
        return invalid_argument;
    }

    span_t received, remainder;
    result_t result = read_until_headers_complete(connection, buffer, &received, &remainder);

    if (is_error(result))
    {
        return result;
    }

    result = http_request_parse(request, received);
    if (is_error(result))
    {
        return result;
    }

    /* Pull body bytes if Content-Length declares them. */
    result = maybe_read_body(connection, &request->headers, &request->body, remainder);
    if (is_error(result))
    {
        return result;
    }

    /* Advance remainder past whatever body bytes were consumed in the buffer. */
    if (out_buffer_remainder != NULL)
    {
        uint32_t body_len = span_get_size(request->body);
        if (body_len <= span_get_size(remainder))
        {
            *out_buffer_remainder = span_slice_to_end(remainder, body_len);
        }
        else
        {
            *out_buffer_remainder = SPAN_EMPTY;
        }
    }

    return ok;
}

result_t http_connection_send_response(http_connection_t* connection, http_response_t* response)
{
    result_t result;

    if (connection == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        result = http_response_serialize_to(response, &connection->stream);
    }

    return result;
}

result_t http_connection_send_request(http_connection_t* connection, http_request_t* request)
{
    result_t result;

    if (connection == NULL || request == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        result = http_request_serialize_to(request, &connection->stream);
    }

    return result;
}

result_t http_connection_receive_response(http_connection_t* connection, span_t buffer, http_response_t* response, span_t* out_buffer_remainder)
{
    if (connection == NULL || response == NULL)
    {
        return invalid_argument;
    }

    span_t received, remainder;
    result_t result = read_until_headers_complete(connection, buffer, &received, &remainder);

    if (is_error(result))
    {
        return result;
    }

    result = http_response_parse(response, received, NULL);
    if (is_error(result))
    {
        return result;
    }

    result = maybe_read_body(connection, &response->headers, &response->body, remainder);
    if (is_error(result))
    {
        return result;
    }

    if (out_buffer_remainder != NULL)
    {
        uint32_t body_len = span_get_size(response->body);
        if (body_len <= span_get_size(remainder))
        {
            *out_buffer_remainder = span_slice_to_end(remainder, body_len);
        }
        else
        {
            *out_buffer_remainder = SPAN_EMPTY;
        }
    }

    return ok;
}

result_t http_connection_close(http_connection_t* connection)
{
    result_t result;

    if (connection == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        // TODO: add a connection_state flag to know if it is initialized.
        result = stream_close(&connection->stream);
        connection->endpoint = NULL;
    }

    return result;
}
