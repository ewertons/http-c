#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>

#include "span.h"
#include "niceties.h"
#include "socketx.h"
#include "socket_stream.h"
#include "base64.h"
#include "logging.h"

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
            endpoint->socket_config.proxy = config->proxy;

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
            else
            {
                /* socket_init allocated a TLS context, and the socket
                 * layer's connect failure path frees only the per-attempt
                 * session and descriptor. Callers do not deinitialize an
                 * object whose creation failed -- and now that a timeout
                 * makes failure an ordinary outcome rather than a rarity,
                 * leaving it would leak a context per stalled address. */
                (void)socket_deinit(&connection->socket);
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

/* ------------------------------------------------------------------------- *
 * Proxy configuration helpers.
 * ------------------------------------------------------------------------- */

static uint32_t text_length(span_t text)
{
    const uint8_t* pointer = span_get_ptr(text);
    uint32_t length = 0;
    while (length < span_get_size(text) && pointer[length] != '\0')
    {
        length++;
    }
    return length;
}

static bool is_blank(uint8_t character)
{
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

static span_t trim_blanks(span_t text)
{
    uint32_t start = 0;
    uint32_t end = span_get_size(text);
    while (start < end && is_blank(span_get(text, start))) start++;
    while (end > start && is_blank(span_get(text, end - 1))) end--;
    return span_slice(text, start, end - start);
}

static uint8_t to_lower_ascii(uint8_t character)
{
    return (character >= 'A' && character <= 'Z') ? (uint8_t)(character - 'A' + 'a') : character;
}

static bool equals_ignoring_case(span_t first, span_t second)
{
    if (span_get_size(first) != span_get_size(second))
    {
        return false;
    }
    for (uint32_t index = 0; index < span_get_size(first); index++)
    {
        if (to_lower_ascii(span_get(first, index)) != to_lower_ascii(span_get(second, index)))
        {
            return false;
        }
    }
    return true;
}

static int find_byte(span_t text, uint8_t wanted)
{
    for (uint32_t index = 0; index < span_get_size(text); index++)
    {
        if (span_get(text, index) == wanted) return (int)index;
    }
    return -1;
}

static int find_last_byte(span_t text, uint8_t wanted)
{
    for (uint32_t index = span_get_size(text); index > 0; index--)
    {
        if (span_get(text, index - 1) == wanted) return (int)(index - 1);
    }
    return -1;
}

static int hexadecimal_value(uint8_t character)
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

/* Percent-decodes `encoded` into `destination` (which may be NULL to only
 * measure). Returns the decoded length, or -1 for a malformed escape. */
static int percent_decode(span_t encoded, uint8_t* destination)
{
    int length = 0;
    for (uint32_t index = 0; index < span_get_size(encoded); index++)
    {
        uint8_t character = span_get(encoded, index);
        if (character == '%')
        {
            if (index + 2 >= span_get_size(encoded) ||
                hexadecimal_value(span_get(encoded, index + 1)) < 0 ||
                hexadecimal_value(span_get(encoded, index + 2)) < 0)
            {
                return -1;
            }
            character = (uint8_t)(hexadecimal_value(span_get(encoded, index + 1)) * 16 +
                                  hexadecimal_value(span_get(encoded, index + 2)));
            index += 2;
        }
        if (destination != NULL)
        {
            destination[length] = character;
        }
        length++;
    }
    return length;
}

static bool parse_port(span_t digits, int* out_port)
{
    uint32_t port = 0;
    if (span_is_empty(digits) || span_get_size(digits) > 5)
    {
        return false;
    }
    for (uint32_t index = 0; index < span_get_size(digits); index++)
    {
        uint8_t character = span_get(digits, index);
        if (character < '0' || character > '9') return false;
        port = port * 10u + (uint32_t)(character - '0');
    }
    if (port == 0 || port > 65535u)
    {
        return false;
    }
    *out_port = (int)port;
    return true;
}

static bool is_valid_hostname(span_t hostname)
{
    if (span_is_empty(hostname))
    {
        return false;
    }
    for (uint32_t index = 0; index < span_get_size(hostname); index++)
    {
        uint8_t character = span_get(hostname, index);
        if (character <= ' ' || character >= 0x7f || character == '/' || character == '@' ||
            character == '[' || character == ']' || character == '?' || character == '#')
        {
            return false;
        }
    }
    return true;
}

result_t http_endpoint_proxy_parse_url(span_t proxy_url, span_t storage, proxy_host_config_t* out_proxy)
{
    static const span_t scheme_separator = span_from_str_literal("://");
    static const span_t http_scheme = span_from_str_literal("http");
    static const span_t basic_prefix = span_from_str_literal("Basic ");
    const int http_default_port = 80;

    if (out_proxy == NULL)
    {
        return invalid_argument;
    }
    (void)memset(out_proxy, 0, sizeof(*out_proxy));

    span_t url = trim_blanks(span_slice(proxy_url, 0, text_length(proxy_url)));
    if (span_is_empty(url))
    {
        return invalid_argument;
    }

    span_t after_scheme;
    int scheme_end = span_find(url, 0, scheme_separator, &after_scheme);
    if (scheme_end >= 0)
    {
        if (!equals_ignoring_case(span_slice(url, 0, (uint32_t)scheme_end), http_scheme))
        {
            log_error("proxy URL: only the http scheme is supported");
            return invalid_argument;
        }
        url = after_scheme;
    }

    /* The authority ends at the first '/', '?' or '#'; whatever follows is
     * meaningless for a proxy and ignored. */
    uint32_t authority_length = 0;
    while (authority_length < span_get_size(url))
    {
        uint8_t character = span_get(url, authority_length);
        if (character == '/' || character == '?' || character == '#') break;
        authority_length++;
    }
    span_t authority = span_slice(url, 0, authority_length);

    span_t user_information = SPAN_EMPTY;
    bool has_user_information = false;
    int at_position = find_last_byte(authority, '@');
    if (at_position >= 0)
    {
        has_user_information = true;
        user_information = span_slice(authority, 0, (uint32_t)at_position);
        authority = span_slice_to_end(authority, (uint32_t)at_position + 1);
    }

    span_t hostname;
    span_t port_digits = SPAN_EMPTY;
    if (!span_is_empty(authority) && span_get(authority, 0) == '[')
    {
        int closing = find_byte(authority, ']');
        if (closing < 2)
        {
            return invalid_argument;
        }
        hostname = span_slice(authority, 1, (uint32_t)closing - 1);
        span_t after = span_slice_to_end(authority, (uint32_t)closing + 1);
        if (!span_is_empty(after))
        {
            if (span_get(after, 0) != ':') return invalid_argument;
            port_digits = span_slice_to_end(after, 1);
        }
        /* An IPv6 literal is made of hex digits, ':' and '.' (and a zone). */
        for (uint32_t index = 0; index < span_get_size(hostname); index++)
        {
            uint8_t character = span_get(hostname, index);
            if (!(hexadecimal_value(character) >= 0 || character == ':' || character == '.' || character == '%'))
            {
                return invalid_argument;
            }
        }
    }
    else
    {
        int colon = find_byte(authority, ':');
        if (colon >= 0)
        {
            if (find_last_byte(authority, ':') != colon)
            {
                /* An unbracketed IPv6 literal cannot be told from a port. */
                return invalid_argument;
            }
            hostname = span_slice(authority, 0, (uint32_t)colon);
            port_digits = span_slice_to_end(authority, (uint32_t)colon + 1);
        }
        else
        {
            hostname = authority;
        }
        if (!is_valid_hostname(hostname))
        {
            return invalid_argument;
        }
    }

    if (span_is_empty(hostname))
    {
        return invalid_argument;
    }

    int port = http_default_port;
    if (!span_is_empty(port_digits) && !parse_port(port_digits, &port))
    {
        return invalid_argument;
    }

    span_t user = user_information;
    span_t password = SPAN_EMPTY;
    if (has_user_information)
    {
        int colon = find_byte(user_information, ':');
        if (colon >= 0)
        {
            user = span_slice(user_information, 0, (uint32_t)colon);
            password = span_slice_to_end(user_information, (uint32_t)colon + 1);
        }
    }

    int user_length = percent_decode(user, NULL);
    int password_length = percent_decode(password, NULL);
    if (user_length < 0 || password_length < 0)
    {
        return invalid_argument;
    }

    /* Layout: hostname NUL ["Basic " base64 NUL] ... [user:password]
     * The decoded credentials are staged at the very end of storage, past
     * where the encoding is written, and wiped once encoded. */
    uint8_t* base = span_get_ptr(storage);
    uint32_t capacity = span_get_size(storage);
    uint32_t hostname_length = span_get_size(hostname);
    uint32_t needed = hostname_length + 1;
    uint32_t credentials_length = 0;
    uint32_t encoded_length = 0;

    if (has_user_information)
    {
        credentials_length = (uint32_t)user_length + 1u + (uint32_t)password_length;
        encoded_length = 4u * ((credentials_length + 2u) / 3u);
        needed += span_get_size(basic_prefix) + encoded_length + 1u + credentials_length;
    }

    if (base == NULL || capacity < needed)
    {
        return insufficient_size;
    }

    (void)memcpy(base, span_get_ptr(hostname), hostname_length);
    base[hostname_length] = '\0';
    span_t stored_hostname = span_init(base, hostname_length);
    span_t stored_authorization = SPAN_EMPTY;

    if (has_user_information)
    {
        uint8_t* credentials = base + capacity - credentials_length;
        (void)percent_decode(user, credentials);
        credentials[user_length] = ':';
        (void)percent_decode(password, credentials + user_length + 1);

        uint8_t* authorization = base + hostname_length + 1;
        (void)memcpy(authorization, span_get_ptr(basic_prefix), span_get_size(basic_prefix));

        span_t encoded;
        int encode_result = base64_encode(span_init(credentials, credentials_length),
                                          span_init(authorization + span_get_size(basic_prefix), encoded_length),
                                          &encoded);
        (void)memset(credentials, 0, credentials_length);
        if (encode_result != 0)
        {
            (void)memset(base, 0, needed - credentials_length);
            return error;
        }

        uint32_t authorization_length = span_get_size(basic_prefix) + encoded_length;
        authorization[authorization_length] = '\0';
        stored_authorization = span_init(authorization, authorization_length);
    }

    out_proxy->hostname = stored_hostname;
    out_proxy->port = port;
    out_proxy->authorization = stored_authorization;
    return ok;
}

bool http_endpoint_proxy_should_bypass(span_t no_proxy, span_t hostname)
{
    hostname = trim_blanks(span_slice(hostname, 0, text_length(hostname)));
    if (span_get_size(hostname) >= 2 && span_get(hostname, 0) == '[' &&
        span_get(hostname, span_get_size(hostname) - 1) == ']')
    {
        hostname = span_slice(hostname, 1, span_get_size(hostname) - 2);
    }
    if (!span_is_empty(hostname) && span_get(hostname, span_get_size(hostname) - 1) == '.')
    {
        hostname = span_slice(hostname, 0, span_get_size(hostname) - 1);
    }
    if (span_is_empty(hostname))
    {
        return false;
    }

    span_t remaining = span_slice(no_proxy, 0, text_length(no_proxy));
    while (!span_is_empty(remaining))
    {
        int comma = find_byte(remaining, ',');
        span_t entry = comma >= 0 ? span_slice(remaining, 0, (uint32_t)comma) : remaining;
        remaining = comma >= 0 ? span_slice_to_end(remaining, (uint32_t)comma + 1) : SPAN_EMPTY;

        entry = trim_blanks(entry);
        if (span_is_empty(entry))
        {
            continue;
        }
        if (span_get_size(entry) == 1 && span_get(entry, 0) == '*')
        {
            return true;
        }

        if (span_get(entry, 0) == '[')
        {
            int closing = find_byte(entry, ']');
            if (closing < 2) continue;
            entry = span_slice(entry, 1, (uint32_t)closing - 1);
        }
        else
        {
            /* "host:port" -- but leave a bare IPv6 literal alone. */
            int colon = find_byte(entry, ':');
            if (colon >= 0 && find_last_byte(entry, ':') == colon)
            {
                entry = span_slice(entry, 0, (uint32_t)colon);
            }
            if (span_get_size(entry) >= 2 && span_get(entry, 0) == '*' && span_get(entry, 1) == '.')
            {
                entry = span_slice_to_end(entry, 1);
            }
            if (!span_is_empty(entry) && span_get(entry, 0) == '.')
            {
                entry = span_slice_to_end(entry, 1);
            }
            if (!span_is_empty(entry) && span_get(entry, span_get_size(entry) - 1) == '.')
            {
                entry = span_slice(entry, 0, span_get_size(entry) - 1);
            }
        }
        if (span_is_empty(entry))
        {
            continue;
        }

        if (equals_ignoring_case(hostname, entry))
        {
            return true;
        }
        uint32_t host_size = span_get_size(hostname);
        uint32_t entry_size = span_get_size(entry);
        if (host_size > entry_size &&
            span_get(hostname, host_size - entry_size - 1) == '.' &&
            equals_ignoring_case(span_slice_to_end(hostname, host_size - entry_size), entry))
        {
            return true;
        }
    }

    return false;
}

/* The first of the named variables that is set and not empty. */
static const char* first_environment_value(const char* first_name, const char* second_name)
{
    const char* value = getenv(first_name);
    if (value == NULL || value[0] == '\0')
    {
        value = getenv(second_name);
    }
    return (value == NULL || value[0] == '\0') ? NULL : value;
}

result_t http_endpoint_config_set_proxy_from_environment(http_endpoint_config_t* configuration, span_t storage)
{
    if (configuration == NULL)
    {
        return invalid_argument;
    }

    (void)memset(&configuration->proxy, 0, sizeof(configuration->proxy));

    const char* proxy_url = configuration->tls.enable
                                ? first_environment_value("https_proxy", "HTTPS_PROXY")
                                : first_environment_value("http_proxy", "HTTP_PROXY");
    if (proxy_url == NULL)
    {
        return ok;
    }

    const char* no_proxy = first_environment_value("no_proxy", "NO_PROXY");
    if (no_proxy != NULL &&
        http_endpoint_proxy_should_bypass(span_init((uint8_t*)(uintptr_t)no_proxy, (uint32_t)strlen(no_proxy)),
                                          configuration->remote.hostname))
    {
        return ok;
    }

    result_t result = http_endpoint_proxy_parse_url(span_init((uint8_t*)(uintptr_t)proxy_url, (uint32_t)strlen(proxy_url)),
                                                    storage, &configuration->proxy);
    if (result == invalid_argument)
    {
        /* Not the value itself: it may carry credentials. */
        log_error("http_endpoint: the %s proxy URL in the environment is malformed or unsupported",
                  configuration->tls.enable ? "https_proxy" : "http_proxy");
    }
    return result;
}
