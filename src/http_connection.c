#include <stdlib.h>

#include "span.h"
#include "niceties.h"
#include "socket_stream.h"

#include "http_endpoint.h"
#include "http_connection.h"

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

result_t http_connection_receive_request(http_connection_t* connection, span_t buffer, http_request_t* request, span_t* out_buffer_remainder)
{
    result_t result;

    if (connection == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        span_t original_buffer = buffer;
        span_t bytes_read;
        
        while (succeeded(result = stream_read(&connection->stream, buffer, &bytes_read, &buffer)))
        {
            bytes_read = span_slice_out(original_buffer, buffer);

            if (span_find_reverse(bytes_read, -1, headers_terminator) != -1)
            {
                result = http_request_parse(request, bytes_read);

                if (out_buffer_remainder != NULL)
                {
                    *out_buffer_remainder = buffer;
                }

                break;
            }
        }
    }

    return result;
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
        result = ok;
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

result_t http_connection_receive_response(http_connection_t* connection, http_response_t* response)
{
    result_t result;

    if (connection == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        result = ok;
    }

    return result;
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
        result = ok;
    }

    return result;
}
