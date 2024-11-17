#include <stdlib.h>

#include "span.h"
#include "niceties.h"
#include "socket.h"

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
        endpoint->socket_config = socket_get_default_secure_listener_config();
        endpoint->socket_config.local.port = config->port;
        endpoint->socket_config.tls.enable = config->tls.enable;
        endpoint->socket_config.tls.certificate_file = config->tls.certificate_file;
        endpoint->socket_config.tls.private_key_file = config->tls.private_key_file;
        
        result = socket_init(&endpoint->socket, &endpoint->socket_config);
    }

    return result;
}

result_t http_endpoint_wait_for_connection(http_endpoint_t* endpoint, http_connection_t* connection)
{
    result_t result;

    if (endpoint == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        result = ok;
    }

    return result;
}

typedef struct async_arg
{
    http_endpoint_t* endpoint;
    http_connection_t* connection;
} async_arg_t;

static result_t internal_wait_for_connection_async(void* user_args, task_t* my_task)
{
    async_arg_t* arg = (async_arg_t*)user_args;

    return http_endpoint_wait_for_connection(arg->endpoint, arg->connection);
}

task_t* http_endpoint_wait_for_connection_async(http_endpoint_t* endpoint, http_connection_t* connection)
{
    async_arg_t arg;
    arg.endpoint = endpoint;
    arg.connection = connection;

    return task_run(internal_wait_for_connection_async, &arg);
}


result_t http_endpoint_connect(http_endpoint_t* endpoint, http_connection_t* connection)
{
    result_t result;

    if (endpoint == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        result = ok;
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
        result = ok;
    }

    return result;
}