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
 * try_again instead of parking in recv forever; the deadline itself lives in
 * the read loops and this only guarantees they get to run.
 *
 * Receive only: a send timeout would abandon a request half-sent, which is
 * worse than a slow one. */
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
            /* The socket layer applies this before the TLS handshake, which
             * apply_io_slice below cannot reach: by the time a connection
             * exists the handshake has already happened, or hung. */
            endpoint->socket_config.io_timeout_ms = config->io_timeout_ms;

            /* No socket_init on this path -- the socket that gets used
             * belongs to the http_connection -- so the memset leaves both
             * descriptors at 0, a real one rather than a sentinel.
             * socket_deinit closes anything but -1, so teardown used to close
             * fd 0: stdin first, then whichever socket inherited the number,
             * hanging whoever owned it. */
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
