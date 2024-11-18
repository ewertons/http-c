#include <stdlib.h>

#include "span.h"
#include "niceties.h"

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

result_t http_connection_receive_request(http_connection_t* connection, http_request_t* request)
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
