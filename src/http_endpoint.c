#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>

#include "span.h"
#include "niceties.h"
#include "socketx.h"
#include "socket_stream.h"

#include "http_endpoint.h"

/* Give the descriptor a short receive timeout so a stalled read surfaces as
 * try_again instead of parking in recv forever. The deadline itself lives in
 * the read loops; this only guarantees they get to run.
 *
 * Receive only. A send timeout would turn a merely slow peer into a failed
 * write halfway through a request, and a half-sent request is worse than a
 * slow one. */
static void apply_io_slice(socket_t* socket, uint32_t io_timeout_ms)
{
    if (io_timeout_ms == 0 || socket->sd < 0)
    {
        return;
    }

    uint32_t slice = io_timeout_ms < HTTP_IO_POLL_SLICE_MS ? io_timeout_ms
                                                           : HTTP_IO_POLL_SLICE_MS;
    struct timeval tv;
    tv.tv_sec  = (time_t)(slice / 1000u);
    tv.tv_usec = (suseconds_t)((slice % 1000u) * 1000u);

    (void)setsockopt(socket->sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

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
        endpoint->io_timeout_ms = config->io_timeout_ms;

        if (endpoint->role == http_endpoint_server)
        {
            endpoint->socket_config = socket_get_default_secure_server_config();
            endpoint->socket_config.local = config->local;
            endpoint->socket_config.tls.enable = config->tls.enable;
            endpoint->socket_config.tls.certificate_file = config->tls.certificate_file;
            endpoint->socket_config.tls.private_key_file = config->tls.private_key_file;
            
            result = socket_init(&endpoint->socket, &endpoint->socket_config);
        }
        else // http_endpoint_client
        {
            endpoint->socket_config = socket_get_default_secure_client_config();
            endpoint->socket_config.remote = config->remote;
            endpoint->socket_config.tls.enable = config->tls.enable;
            endpoint->socket_config.tls.certificate_file = config->tls.certificate_file;
            endpoint->socket_config.tls.private_key_file = config->tls.private_key_file;
            endpoint->socket_config.tls.trusted_certificate_file = config->tls.trusted_certificate_file;

            /* A client endpoint owns no descriptor of its own -- the socket
             * belongs to the http_connection that http_endpoint_connect
             * initializes. socket_init is therefore not called here, and
             * without it nothing resets these two fields: the memset above
             * leaves both at 0, which is not a sentinel but a real
             * descriptor.
             *
             * http_endpoint_deinit passes this socket to socket_deinit,
             * which closes every descriptor that is not -1. So each client
             * endpoint teardown closed fd 0 -- first stdin, and after that
             * whichever socket the kernel had since handed the lowest free
             * number to. The victim was usually a live connection owned by
             * another thread, which then hung forever waiting for a reply
             * that could no longer be written to it.
             */
            endpoint->socket.sd        = -1;
            endpoint->socket.listen_sd = -1;

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

        result = socket_accept(&endpoint->socket, &connection->socket);
        if (is_success(result))
        {
             connection->io_timeout_ms = endpoint->io_timeout_ms;
             apply_io_slice(&connection->socket, endpoint->io_timeout_ms);
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
        return task_run(internal_wait_for_connection_async, connection);
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
            result = socket_connect(&connection->socket);
            if (is_success(result))
            {
                connection->io_timeout_ms = endpoint->io_timeout_ms;
                apply_io_slice(&connection->socket, endpoint->io_timeout_ms);
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
