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

    /* Optional HTTP CONNECT proxy for a client; ignored for a server. An
     * empty hostname (the default) connects directly. Handed to the socket
     * layer unchanged -- see proxy_host_config_t in socketx.h: the remote
     * name is resolved by the proxy, TLS runs end-to-end through the tunnel
     * and is verified against remote.hostname, and the spans must outlive
     * the endpoint. http_endpoint_config_set_proxy_from_environment fills
     * it from the usual environment variables. */
    proxy_host_config_t proxy;

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

/* ------------------------------------------------------------------------- *
 * Proxy configuration helpers.
 *
 * None of these allocate: every string they produce is written into the
 * caller's `storage` (NUL-terminated, so it can also be logged or passed to C
 * APIs), and the resulting spans point into it. Keep `storage` alive for as
 * long as the configuration -- and any endpoint initialised from it -- is in
 * use. 512 bytes is plenty for any ordinary proxy URL.
 * ------------------------------------------------------------------------- */

/* Parses a proxy URL of the form
 *
 *     [http://][user[:password]@]host[:port][/]
 *
 * into out_proxy. host may be a name, an IPv4 literal or a bracketed IPv6
 * literal (stored without the brackets). Without a port the scheme's default
 * applies, which for http is 80. user and password may be percent-encoded;
 * when present they become a "Basic <base64(user:password)>" authorization.
 * Only the http scheme is accepted (TLS to the proxy itself is not
 * supported); a URL with no scheme is taken as http.
 *
 * Returns ok, invalid_argument for a malformed or unsupported URL, or
 * insufficient_size when storage is too small. out_proxy is cleared on
 * failure. */
result_t http_endpoint_proxy_parse_url(span_t proxy_url, span_t storage, proxy_host_config_t* out_proxy);

/* Whether `hostname` is excluded from proxying by a no_proxy-style list:
 * comma-separated entries, surrounding blanks ignored, matched
 * case-insensitively. "*" matches every host; any other entry matches the
 * host itself and its subdomains, with or without a leading "." (so both
 * "example.com" and ".example.com" match "example.com" and
 * "api.example.com", but not "badexample.com"). A ":port" suffix on an entry
 * is ignored. CIDR ranges are not supported. */
bool http_endpoint_proxy_should_bypass(span_t no_proxy, span_t hostname);

/* Sets configuration->proxy from the environment, the way curl does:
 *
 *   - https_proxy, then HTTPS_PROXY, when configuration->tls.enable is set;
 *     http_proxy, then HTTP_PROXY, otherwise (the first non-empty one wins);
 *   - no proxy when configuration->remote.hostname is listed in no_proxy
 *     (or NO_PROXY), see http_endpoint_proxy_should_bypass;
 *   - the value is parsed by http_endpoint_proxy_parse_url into storage.
 *
 * Set configuration->remote and configuration->tls.enable first. The proxy
 * is cleared when the environment names none, and on failure.
 *
 * Returns ok (with or without a proxy), invalid_argument for a NULL
 * configuration or a malformed proxy URL, or insufficient_size. */
result_t http_endpoint_config_set_proxy_from_environment(http_endpoint_config_t* configuration, span_t storage);

#endif // HTTP_ENDPOINT_H
