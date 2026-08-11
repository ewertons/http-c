#ifndef HTTP_ENDPOINT_H
#define HTTP_ENDPOINT_H

#include <span.h>

#include "niceties.h"
#include "socketx.h"
#include "task.h"

typedef enum http_endpoint_role
{
    http_endpoint_client,
    http_endpoint_server
} http_endpoint_role_t;

/* Ceiling on a single blocking exchange: how long a read may make no progress
 * before it is abandoned. The budget refreshes whenever bytes arrive, so a
 * large body is not penalised -- it bounds silence, not duration.
 *
 * Needed because a peer that completes the handshake and then stops talking
 * would otherwise own the calling thread for the life of the process. */
#define HTTP_DEFAULT_IO_TIMEOUT_MS 30000u

/* How long a receive blocks before reporting try_again. Applied with
 * SO_RCVTIMEO, and short on purpose: it is the loop's heartbeat, not the
 * deadline. A blocking socket otherwise sits in recv forever and never gives
 * the deadline a chance to be checked. */
#define HTTP_IO_POLL_SLICE_MS 50u

typedef struct http_endpoint_config
{
    http_endpoint_role_t role;

    local_host_config_t local;
    remote_host_config_t remote;

    /* Milliseconds a read may make no progress before the exchange fails.
     * 0 waits forever, which is the behaviour this replaced.
     *
     * On a client it is also handed to the socket layer, which spends it on
     * every step of reaching a peer: the TCP connect, the TLS handshake,
     * and each blocking receive. None of those had a bound of their own --
     * an address can complete the TCP handshake and then never speak, and
     * one that swallows SYNs is otherwise left to tcp_syn_retries.
     *
     * It is a budget per step, not for the exchange as a whole.
     *
     * Only affects the blocking API. The non-blocking path reports try_again
     * to its caller, whose event loop already owns the timing. */
    uint32_t io_timeout_ms;

    struct
    {
        bool enable;
        const char* certificate_file;
        const char* private_key_file;
        const char* trusted_certificate_file;
    } tls;
} http_endpoint_config_t;

typedef struct http_endpoint
{
    http_endpoint_role_t role;
    socket_config_t socket_config;
    socket_t socket;
    uint32_t io_timeout_ms;
} http_endpoint_t;

#include "http_connection.h"

static inline http_endpoint_config_t http_endpoint_get_default_secure_server_config()
{
    http_endpoint_config_t config = { 0 };
    config.role = http_endpoint_server;
    config.tls.enable = true;
    config.local.port = DEFAULT_LISTENING_PORT;
    /* Not defaulted for a listener. Between requests on a kept-alive
     * connection a server is idle by design, and a deadline here would drop
     * those on a timer. A server that wants slow-client protection should set
     * it explicitly. */
    config.io_timeout_ms = 0;
    return config;
}

static inline http_endpoint_config_t http_endpoint_get_default_secure_client_config()
{
    http_endpoint_config_t config = { 0 };
    config.role = http_endpoint_client;
    config.tls.enable = true;
    config.io_timeout_ms = HTTP_DEFAULT_IO_TIMEOUT_MS;
    return config;
}

result_t http_endpoint_init(http_endpoint_t* endpoint, http_endpoint_config_t* config);

result_t http_endpoint_wait_for_connection(http_endpoint_t* endpoint, http_connection_t* connection);
task_t* http_endpoint_wait_for_connection_async(http_endpoint_t* endpoint, http_connection_t* connection);

result_t http_endpoint_connect(http_endpoint_t* endpoint, http_connection_t* connection);

result_t http_endpoint_deinit(http_endpoint_t* endpoint);

#endif // HTTP_ENDPOINT_H
