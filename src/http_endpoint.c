#include <stdlib.h>
#include <string.h>

#include "span.h"
#include "niceties.h"
#include "socket.h"
#include "socket_stream.h"

#include "http_endpoint.h"

result_t http_endpoint_init(http_endpoint_t* endpoint, http_endpoint_config_t* config)
{
    result_t result;

    if (endpoint == NULL || config == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        (void)memset(endpoint, 0, sizeof(http_endpoint_t));
        endpoint->role = config->role;

        if (endpoint->role == http_endpoint_server)
        {
            endpoint->socket_config = socket_get_default_secure_server_config();
            endpoint->socket_config.local = config->local;
            endpoint->socket_config.tls.certificate_file = config->tls.certificate_file;
            endpoint->socket_config.tls.private_key_file = config->tls.private_key_file;
            
            result = socket_init(&endpoint->socket, &endpoint->socket_config);
        }
        else // http_endpoint_client
        {
            endpoint->socket_config = socket_get_default_secure_client_config();
            endpoint->socket_config.remote = config->remote;
            endpoint->socket_config.tls.certificate_file = config->tls.certificate_file;
            endpoint->socket_config.tls.private_key_file = config->tls.private_key_file;
            endpoint->socket_config.tls.trusted_certificate_file = config->tls.trusted_certificate_file;

            result = ok;
        }
    }

    return result;
}

result_t http_endpoint_wait_for_connection(http_endpoint_t* endpoint, http_connection_t* connection)
{
    result_t result;

    if (endpoint == NULL || endpoint->role != http_endpoint_server)
    {
        result = invalid_argument;
    }
    else
    {
        (void)memset(connection, 0, sizeof(http_connection_t));

        if (is_success(result = socket_accept(&endpoint->socket, &connection->socket)))
        {
             result = socket_stream_initialize(&connection->stream, &connection->socket);
        }
    }

    return result;
}

static result_t internal_wait_for_connection_async(void* user_args, task_t* my_task)
{
    http_connection_t* connection = (http_connection_t*)user_args;
    http_endpoint_t* endpoint;

    result_t result = http_connection_get_endpoint(connection, &endpoint);

    if (is_error(result))
    {
        return result;
    }
    else
    {
        return http_endpoint_wait_for_connection(endpoint, connection);
    }
}

task_t* http_endpoint_wait_for_connection_async(http_endpoint_t* endpoint, http_connection_t* connection)
{
    if (endpoint == NULL || connection == NULL)
    {
        return NULL;
    }
    else if (is_error(http_connection_set_endpoint(connection, endpoint)))
    {
        return NULL;
    }
    else
    {
        // TODO: pass a cancellation handler.
        return task_run(internal_wait_for_connection_async, NULL, connection);
    }
}

result_t http_endpoint_connect(http_endpoint_t* endpoint, http_connection_t* connection)
{
    result_t result;

    if (endpoint == NULL || endpoint->role != http_endpoint_client)
    {
        result = invalid_argument;
    }
    else
    {
        (void)memset(connection, 0, sizeof(http_connection_t));

        result = socket_init(&connection->socket, &endpoint->socket_config);
        
        if (is_success(result))
        {
            if (is_success(result = socket_connect(&connection->socket)))
            {
                result = socket_stream_initialize(&connection->stream, &connection->socket);
            }
        }
    }

    return result;
}

result_t http_endpoint_deinit(http_endpoint_t* endpoint)
{
    result_t result;

    if (endpoint == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        result = socket_deinit(&endpoint->socket);
    }

    return result;
}
