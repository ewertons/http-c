/* Client-side response handling: body framing (Content-Length, chunked,
 * close-delimited, bodiless statuses and HEAD), streaming bodies of any size,
 * and reaching a server through an HTTP CONNECT proxy, plus the proxy
 * configuration helpers.
 *
 * Everything runs on loopback. The servers here are scripted raw sockets so
 * each test controls the exact bytes on the wire. */

#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <cmocka.h>

#include "niceties.h"
#include "task.h"

#include "http_endpoint.h"
#include "http_connection.h"
#include "http_headers.h"
#include "http_methods.h"
#include "http_codes.h"
#include "http_request.h"
#include "http_response.h"
#include "http_versions.h"

#include <test_http.h>

#define LARGE_BODY_SIZE (5u * 1024u * 1024u)
#define PROXY_TEST_SERVER_PORT 4361

/* ------------------------------------------------------------------------- *
 * Fixtures.
 * ------------------------------------------------------------------------- */

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)(now.tv_nsec / 1000000);
}

static int loopback_listener(uint16_t* out_port)
{
    int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    assert_true(descriptor >= 0);

    struct sockaddr_in address;
    (void)memset(&address, 0, sizeof(address));
    address.sin_family      = AF_INET;
    address.sin_port        = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    assert_int_equal(bind(descriptor, (struct sockaddr*)&address, sizeof(address)), 0);
    assert_int_equal(listen(descriptor, 4), 0);

    socklen_t length = sizeof(address);
    assert_int_equal(getsockname(descriptor, (struct sockaddr*)&address, &length), 0);
    *out_port = ntohs(address.sin_port);
    return descriptor;
}

static bool send_all(int descriptor, const void* data, size_t length)
{
    const uint8_t* cursor = (const uint8_t*)data;
    while (length > 0)
    {
        ssize_t sent = send(descriptor, cursor, length, MSG_NOSIGNAL);
        if (sent <= 0)
        {
            return false;
        }
        cursor += sent;
        length -= (size_t)sent;
    }
    return true;
}

static bool send_text(int descriptor, const char* text)
{
    return send_all(descriptor, text, strlen(text));
}

/* Waits for the peer to close (or a 10 s safety net). */
static void drain_until_closed(int descriptor)
{
    char discard[256];
    for (;;)
    {
        struct pollfd poll_descriptor = { .fd = descriptor, .events = POLLIN, .revents = 0 };
        if (poll(&poll_descriptor, 1, 10000) <= 0 ||
            recv(descriptor, discard, sizeof(discard), 0) <= 0)
        {
            return;
        }
    }
}

static uint8_t pattern_byte(uint64_t position)
{
    return (uint8_t)((position % 251u) ^ (position >> 16));
}

typedef enum large_body_framing
{
    large_body_framing_content_length,
    large_body_framing_chunked,
    large_body_framing_until_close,
} large_body_framing_t;

typedef struct scripted_server scripted_server_t;
typedef void (*scripted_response_writer_t)(scripted_server_t* server, int connection);

struct scripted_server
{
    int listener;
    uint16_t port;
    pthread_t thread;
    scripted_response_writer_t writer;
    const char* response_text;
    large_body_framing_t large_body_framing;
    bool keep_open_until_client_closes;
    char request[4096];
    size_t request_length;
};

static void* scripted_server_run(void* argument)
{
    scripted_server_t* server = (scripted_server_t*)argument;

    int connection = accept(server->listener, NULL, NULL);
    if (connection < 0)
    {
        return NULL;
    }

    while (server->request_length < sizeof(server->request) - 1 &&
           strstr(server->request, "\r\n\r\n") == NULL)
    {
        ssize_t received = recv(connection, server->request + server->request_length,
                                sizeof(server->request) - 1 - server->request_length, 0);
        if (received <= 0)
        {
            break;
        }
        server->request_length += (size_t)received;
    }

    server->writer(server, connection);

    if (server->keep_open_until_client_closes)
    {
        drain_until_closed(connection);
    }
    close(connection);
    return NULL;
}

static void write_response_text(scripted_server_t* server, int connection)
{
    (void)send_text(connection, server->response_text);
}

static void write_large_response(scripted_server_t* server, int connection)
{
    static uint8_t piece[80000];
    char header[256];

    switch (server->large_body_framing)
    {
        case large_body_framing_content_length:
            (void)snprintf(header, sizeof(header),
                           "HTTP/1.1 200 OK\r\nX-Test: large\r\nContent-Length: %u\r\n\r\n", LARGE_BODY_SIZE);
            break;
        case large_body_framing_chunked:
            (void)snprintf(header, sizeof(header),
                           "HTTP/1.1 200 OK\r\nX-Test: large\r\nTransfer-Encoding: chunked\r\n\r\n");
            break;
        case large_body_framing_until_close:
            (void)snprintf(header, sizeof(header),
                           "HTTP/1.1 200 OK\r\nX-Test: large\r\nConnection: close\r\n\r\n");
            break;
    }
    if (!send_text(connection, header))
    {
        return;
    }

    static const uint32_t chunk_sizes[] = { 1, 7, 100, 4096, 65536, 70001, 3, 16384, 999 };
    uint64_t position = 0;
    uint32_t chunk_index = 0;

    while (position < LARGE_BODY_SIZE)
    {
        uint32_t size = chunk_sizes[chunk_index % (sizeof(chunk_sizes) / sizeof(chunk_sizes[0]))];
        if (position + size > LARGE_BODY_SIZE)
        {
            size = (uint32_t)(LARGE_BODY_SIZE - position);
        }
        for (uint32_t index = 0; index < size; index++)
        {
            piece[index] = pattern_byte(position + index);
        }

        if (server->large_body_framing == large_body_framing_chunked)
        {
            char size_line[64];
            /* An extension on every other chunk, which must be ignored. */
            (void)snprintf(size_line, sizeof(size_line),
                           chunk_index % 2 == 0 ? "%x\r\n" : "%X;name=value\r\n", size);
            if (!send_text(connection, size_line) ||
                !send_all(connection, piece, size) ||
                !send_text(connection, "\r\n"))
            {
                return;
            }
        }
        else if (!send_all(connection, piece, size))
        {
            return;
        }

        position += size;
        chunk_index++;
    }

    if (server->large_body_framing == large_body_framing_chunked)
    {
        (void)send_text(connection, "0\r\nX-Trailer: one\r\nX-Other: two\r\n\r\n");
    }
}

static void scripted_server_launch(scripted_server_t* server, scripted_response_writer_t writer,
                                   const char* response_text, large_body_framing_t large_body_framing,
                                   bool keep_open_until_client_closes)
{
    (void)memset(server, 0, sizeof(*server));
    server->writer = writer;
    server->response_text = response_text;
    server->large_body_framing = large_body_framing;
    server->keep_open_until_client_closes = keep_open_until_client_closes;
    server->listener = loopback_listener(&server->port);
    assert_int_equal(pthread_create(&server->thread, NULL, scripted_server_run, server), 0);
}

static void scripted_server_start(scripted_server_t* server, scripted_response_writer_t writer,
                                  const char* response_text, bool keep_open_until_client_closes)
{
    scripted_server_launch(server, writer, response_text, large_body_framing_content_length,
                           keep_open_until_client_closes);
}

static void scripted_server_stop(scripted_server_t* server)
{
    (void)shutdown(server->listener, SHUT_RDWR);
    assert_int_equal(pthread_join(server->thread, NULL), 0);
    close(server->listener);
}

/* A minimal CONNECT proxy: one connection, records the request, dials
 * 127.0.0.1:target_port whatever host was asked for, answers 200 and relays. */
typedef struct connect_proxy
{
    int listener;
    uint16_t port;
    uint16_t target_port;
    pthread_t thread;
    char request[2048];
    size_t request_length;
} connect_proxy_t;

static void relay(int first, int second)
{
    uint8_t buffer[16384];
    for (;;)
    {
        struct pollfd descriptors[2] = {
            { .fd = first,  .events = POLLIN, .revents = 0 },
            { .fd = second, .events = POLLIN, .revents = 0 },
        };
        if (poll(descriptors, 2, 10000) <= 0)
        {
            return;
        }
        for (int index = 0; index < 2; index++)
        {
            if (descriptors[index].revents == 0)
            {
                continue;
            }
            int from = index == 0 ? first : second;
            int to   = index == 0 ? second : first;
            ssize_t received = recv(from, buffer, sizeof(buffer), 0);
            if (received <= 0 || !send_all(to, buffer, (size_t)received))
            {
                return;
            }
        }
    }
}

static void* connect_proxy_run(void* argument)
{
    connect_proxy_t* proxy = (connect_proxy_t*)argument;

    int client = accept(proxy->listener, NULL, NULL);
    if (client < 0)
    {
        return NULL;
    }

    while (proxy->request_length < sizeof(proxy->request) - 1 &&
           strstr(proxy->request, "\r\n\r\n") == NULL)
    {
        ssize_t received = recv(client, proxy->request + proxy->request_length,
                                sizeof(proxy->request) - 1 - proxy->request_length, 0);
        if (received <= 0)
        {
            break;
        }
        proxy->request_length += (size_t)received;
    }

    int target = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    (void)memset(&address, 0, sizeof(address));
    address.sin_family      = AF_INET;
    address.sin_port        = htons(proxy->target_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (target >= 0 && connect(target, (struct sockaddr*)&address, sizeof(address)) == 0)
    {
        (void)send_text(client, "HTTP/1.1 200 Connection established\r\n\r\n");
        relay(client, target);
    }
    else
    {
        (void)send_text(client, "HTTP/1.1 502 Bad Gateway\r\n\r\n");
    }

    if (target >= 0)
    {
        close(target);
    }
    close(client);
    return NULL;
}

static void connect_proxy_start(connect_proxy_t* proxy, uint16_t target_port)
{
    (void)memset(proxy, 0, sizeof(*proxy));
    proxy->target_port = target_port;
    proxy->listener = loopback_listener(&proxy->port);
    assert_int_equal(pthread_create(&proxy->thread, NULL, connect_proxy_run, proxy), 0);
}

static void connect_proxy_stop(connect_proxy_t* proxy)
{
    (void)shutdown(proxy->listener, SHUT_RDWR);
    assert_int_equal(pthread_join(proxy->thread, NULL), 0);
    close(proxy->listener);
}

typedef struct test_client
{
    http_endpoint_t endpoint;
    http_connection_t connection;
} test_client_t;

static http_endpoint_config_t plain_client_configuration(uint16_t port)
{
    http_endpoint_config_t configuration = http_endpoint_get_default_secure_client_config();
    configuration.tls.enable      = false;
    configuration.remote.hostname = span_from_str_literal("127.0.0.1");
    configuration.remote.port     = port;
    configuration.io_timeout_ms   = 5000;
    return configuration;
}

static void test_client_connect(test_client_t* client, http_endpoint_config_t* configuration)
{
    assert_int_equal(http_endpoint_init(&client->endpoint, configuration), ok);
    assert_int_equal(http_endpoint_connect(&client->endpoint, &client->connection), ok);
}

static void test_client_close(test_client_t* client)
{
    (void)http_connection_close(&client->connection);
    (void)http_endpoint_deinit(&client->endpoint);
}

static void test_client_send(test_client_t* client, span_t method)
{
    uint8_t header_storage[256];
    http_headers_t headers;
    assert_int_equal(http_headers_init(&headers, span_from_memory(header_storage)), HL_RESULT_OK);
    assert_int_equal(http_headers_add(&headers, HTTP_HEADER_HOST, span_from_str_literal("localhost")), HL_RESULT_OK);

    http_request_t request;
    assert_int_equal(http_request_initialize(&request, method, span_from_str_literal("/"), HTTP_VERSION_1_1, headers), ok);
    assert_int_equal(http_connection_send_request(&client->connection, &request), ok);
}

/* Sends a GET to a scripted server and receives the response into buffer. */
static result_t exchange(scripted_server_t* server, span_t method, span_t buffer,
                         http_response_t* response, span_t* out_remainder)
{
    test_client_t client;
    http_endpoint_config_t configuration = plain_client_configuration(server->port);
    test_client_connect(&client, &configuration);
    test_client_send(&client, method);

    result_t result = http_connection_receive_response(&client.connection, buffer, response, out_remainder);

    test_client_close(&client);
    scripted_server_stop(server);
    return result;
}

static void assert_body_equals(http_response_t* response, const char* expected)
{
    assert_int_equal(span_get_size(response->body), strlen(expected));
    assert_memory_equal(span_get_ptr(response->body), expected, strlen(expected));
}

/* ------------------------------------------------------------------------- *
 * Non-streaming framing.
 * ------------------------------------------------------------------------- */

static void client_reads_a_content_length_body(void** state)
{
    (void)state;
    uint8_t raw[1024];
    scripted_server_t server;
    /* Kept open: the length alone must end the body. */
    scripted_server_start(&server, write_response_text,
                          "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello world", true);

    http_response_t response;
    span_t remainder;
    assert_int_equal(exchange(&server, HTTP_METHOD_GET, span_from_memory(raw), &response, &remainder), ok);
    assert_int_equal(span_compare(response.code, HTTP_CODE_200), 0);
    assert_body_equals(&response, "hello world");
    assert_ptr_equal(span_get_ptr(remainder), span_get_ptr(response.body) + 11);
    assert_ptr_equal(span_get_ptr(remainder) + span_get_size(remainder), raw + sizeof(raw));
}

static void client_reads_a_chunked_body_with_extensions_and_trailers(void** state)
{
    (void)state;
    uint8_t raw[1024];
    scripted_server_t server;
    scripted_server_start(&server, write_response_text,
                          "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                          "5;name=value\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: yes\r\n\r\n", true);

    http_response_t response;
    assert_int_equal(exchange(&server, HTTP_METHOD_GET, span_from_memory(raw), &response, NULL), ok);
    assert_body_equals(&response, "hello world");
}

/* RFC 9112 section 6.3: no Content-Length, not chunked -- the body runs to
 * the connection closing. This used to come back with an empty body. */
static void client_reads_a_body_delimited_by_close(void** state)
{
    (void)state;
    uint8_t raw[1024];
    scripted_server_t server;
    scripted_server_start(&server, write_response_text,
                          "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nall of this is body", false);

    http_response_t response;
    assert_int_equal(exchange(&server, HTTP_METHOD_GET, span_from_memory(raw), &response, NULL), ok);
    assert_body_equals(&response, "all of this is body");
}

/* Bodiless responses must return at once, not wait for a close that a
 * keep-alive server never sends. */
static void assert_bodiless_response_returns_promptly(const char* response_text, span_t method)
{
    uint8_t raw[1024];
    scripted_server_t server;
    scripted_server_start(&server, write_response_text, response_text, true);

    http_response_t response;
    uint64_t started = monotonic_milliseconds();
    assert_int_equal(exchange(&server, method, span_from_memory(raw), &response, NULL), ok);
    assert_true(monotonic_milliseconds() - started < 2000);
    assert_true(span_is_empty(response.body));
}

static void client_reads_no_body_for_204(void** state)
{
    (void)state;
    assert_bodiless_response_returns_promptly("HTTP/1.1 204 No Content\r\nServer: test\r\n\r\n", HTTP_METHOD_GET);
}

static void client_reads_no_body_for_304(void** state)
{
    (void)state;
    assert_bodiless_response_returns_promptly("HTTP/1.1 304 Not Modified\r\nETag: \"x\"\r\n\r\n", HTTP_METHOD_GET);
}

static void client_reads_no_body_for_a_head_request(void** state)
{
    (void)state;
    assert_bodiless_response_returns_promptly("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n", HTTP_METHOD_HEAD);
}

static void client_reports_insufficient_size_for_an_oversized_close_delimited_body(void** state)
{
    (void)state;
    static char response_text[8192];
    (void)strcpy(response_text, "HTTP/1.1 200 OK\r\n\r\n");
    (void)memset(response_text + strlen(response_text), 'x', 4000);

    uint8_t raw[1024];
    scripted_server_t server;
    scripted_server_start(&server, write_response_text, response_text, false);

    http_response_t response;
    assert_int_equal(exchange(&server, HTTP_METHOD_GET, span_from_memory(raw), &response, NULL), insufficient_size);
}

static void client_reports_insufficient_size_for_an_oversized_content_length_body(void** state)
{
    (void)state;
    static char response_text[8192];
    (void)strcpy(response_text, "HTTP/1.1 200 OK\r\nContent-Length: 4000\r\n\r\n");
    (void)memset(response_text + strlen(response_text), 'x', 4000);

    uint8_t raw[1024];
    scripted_server_t server;
    scripted_server_start(&server, write_response_text, response_text, true);

    http_response_t response;
    assert_int_equal(exchange(&server, HTTP_METHOD_GET, span_from_memory(raw), &response, NULL), insufficient_size);
}

static void client_reports_a_truncated_content_length_body(void** state)
{
    (void)state;
    uint8_t raw[1024];
    scripted_server_t server;
    scripted_server_start(&server, write_response_text,
                          "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly this", false);

    http_response_t response;
    assert_int_equal(exchange(&server, HTTP_METHOD_GET, span_from_memory(raw), &response, NULL), error);
}

static void client_reports_a_malformed_chunk_size(void** state)
{
    (void)state;
    uint8_t raw[1024];
    scripted_server_t server;
    scripted_server_start(&server, write_response_text,
                          "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n0\r\n\r\n", false);

    http_response_t response;
    assert_int_equal(exchange(&server, HTTP_METHOD_GET, span_from_memory(raw), &response, NULL), error);
}

/* ------------------------------------------------------------------------- *
 * Streaming.
 * ------------------------------------------------------------------------- */

typedef struct body_verifier
{
    uint64_t received;
    uint32_t calls;
    uint32_t largest_piece;
    bool mismatch;
    uint32_t fail_after_calls;  /* 0 = never */
    result_t failure;
} body_verifier_t;

static result_t verify_body_piece(void* context, span_t body_chunk)
{
    body_verifier_t* verifier = (body_verifier_t*)context;
    verifier->calls++;
    if (span_get_size(body_chunk) == 0)
    {
        verifier->mismatch = true;
    }
    if (span_get_size(body_chunk) > verifier->largest_piece)
    {
        verifier->largest_piece = span_get_size(body_chunk);
    }
    for (uint32_t index = 0; index < span_get_size(body_chunk) && !verifier->mismatch; index++)
    {
        if (span_get(body_chunk, index) != pattern_byte(verifier->received + index))
        {
            verifier->mismatch = true;
        }
    }
    verifier->received += span_get_size(body_chunk);

    if (verifier->fail_after_calls != 0 && verifier->calls >= verifier->fail_after_calls)
    {
        return verifier->failure;
    }
    return ok;
}

static result_t stream_large_body(large_body_framing_t framing, body_verifier_t* verifier,
                                  http_response_t* response, uint8_t* header_buffer, uint32_t header_buffer_size)
{
    scripted_server_t server;
    scripted_server_launch(&server, write_large_response, NULL, framing,
                           framing != large_body_framing_until_close);

    test_client_t client;
    http_endpoint_config_t configuration = plain_client_configuration(server.port);
    test_client_connect(&client, &configuration);
    test_client_send(&client, HTTP_METHOD_GET);

    result_t result = http_connection_receive_response_streaming(&client.connection,
                                                                 span_init(header_buffer, header_buffer_size),
                                                                 response, verify_body_piece, verifier);
    test_client_close(&client);
    scripted_server_stop(&server);
    return result;
}

static void assert_large_body_streams(large_body_framing_t framing)
{
    static uint8_t header_buffer[16384];
    body_verifier_t verifier;
    (void)memset(&verifier, 0, sizeof(verifier));

    http_response_t response;
    assert_int_equal(stream_large_body(framing, &verifier, &response, header_buffer, sizeof(header_buffer)), ok);

    assert_false(verifier.mismatch);
    assert_int_equal(verifier.received, LARGE_BODY_SIZE);
    assert_true(verifier.calls > 300);  /* many pieces, each bounded by the scratch area */
    assert_true(verifier.largest_piece < sizeof(header_buffer));
    assert_true(span_is_empty(response.body));
    assert_int_equal(span_compare(response.code, HTTP_CODE_200), 0);

    span_t value;
    assert_int_equal(http_headers_find(&response.headers, span_from_str_literal("X-Test"), &value), HL_RESULT_OK);
    assert_int_equal(span_compare(value, span_from_str_literal("large")), 0);
}

static void client_streams_a_large_content_length_body(void** state)
{
    (void)state;
    assert_large_body_streams(large_body_framing_content_length);
}

static void client_streams_a_large_chunked_body(void** state)
{
    (void)state;
    assert_large_body_streams(large_body_framing_chunked);
}

static void client_streams_a_large_close_delimited_body(void** state)
{
    (void)state;
    assert_large_body_streams(large_body_framing_until_close);
}

static void assert_handler_failure_stops_the_transfer(large_body_framing_t framing)
{
    static uint8_t header_buffer[4096];
    body_verifier_t verifier;
    (void)memset(&verifier, 0, sizeof(verifier));
    verifier.fail_after_calls = 3;
    verifier.failure = cancelled;

    http_response_t response;
    assert_int_equal(stream_large_body(framing, &verifier, &response, header_buffer, sizeof(header_buffer)), cancelled);
    assert_int_equal(verifier.calls, 3);
    assert_true(verifier.received < LARGE_BODY_SIZE);
}

static void client_streaming_stops_when_the_handler_fails(void** state)
{
    (void)state;
    assert_handler_failure_stops_the_transfer(large_body_framing_content_length);
    assert_handler_failure_stops_the_transfer(large_body_framing_chunked);
    assert_handler_failure_stops_the_transfer(large_body_framing_until_close);
}

static result_t count_pieces(void* context, span_t body_chunk)
{
    (void)body_chunk;
    (*(uint32_t*)context)++;
    return ok;
}

static void client_streaming_a_bodiless_response_never_calls_the_handler(void** state)
{
    (void)state;
    uint8_t header_buffer[1024];
    scripted_server_t server;
    scripted_server_start(&server, write_response_text, "HTTP/1.1 204 No Content\r\n\r\n", true);

    test_client_t client;
    http_endpoint_config_t configuration = plain_client_configuration(server.port);
    test_client_connect(&client, &configuration);
    test_client_send(&client, HTTP_METHOD_GET);

    uint32_t calls = 0;
    http_response_t response;
    assert_int_equal(http_connection_receive_response_streaming(&client.connection, span_from_memory(header_buffer),
                                                                &response, count_pieces, &calls), ok);
    assert_int_equal(calls, 0);

    test_client_close(&client);
    scripted_server_stop(&server);
}

static void client_streaming_requires_a_handler(void** state)
{
    (void)state;
    http_connection_t connection;
    http_response_t response;
    uint8_t header_buffer[64];
    (void)memset(&connection, 0, sizeof(connection));
    assert_int_equal(http_connection_receive_response_streaming(&connection, span_from_memory(header_buffer),
                                                                &response, NULL, NULL), invalid_argument);
    assert_int_equal(http_connection_receive_response_streaming(NULL, span_from_memory(header_buffer),
                                                                &response, count_pieces, NULL), invalid_argument);
}

/* ------------------------------------------------------------------------- *
 * Through a CONNECT proxy.
 * ------------------------------------------------------------------------- */

static void proxied_exchange_with_an_http_c_server(http_endpoint_config_t* client_configuration,
                                                   connect_proxy_t* proxy)
{
    http_endpoint_t server_endpoint;
    http_endpoint_config_t server_configuration = http_endpoint_get_default_secure_server_config();
    server_configuration.tls.enable = false;
    server_configuration.local.port = PROXY_TEST_SERVER_PORT;
    assert_int_equal(http_endpoint_init(&server_endpoint, &server_configuration), ok);

    http_connection_t server_connection;
    task_t* accept_task = http_endpoint_wait_for_connection_async(&server_endpoint, &server_connection);
    assert_non_null(accept_task);

    test_client_t client;
    test_client_connect(&client, client_configuration);
    assert_true(task_wait(accept_task));
    assert_int_equal(task_get_result(accept_task), ok);
    task_release(accept_task);

    test_client_send(&client, HTTP_METHOD_GET);

    uint8_t server_buffer[1024];
    http_request_t request;
    assert_int_equal(http_connection_receive_request(&server_connection, span_from_memory(server_buffer), &request, NULL), ok);
    assert_int_equal(span_compare(request.method, HTTP_METHOD_GET), 0);

    uint8_t response_header_storage[128];
    http_headers_t response_headers;
    assert_int_equal(http_headers_init(&response_headers, span_from_memory(response_header_storage)), HL_RESULT_OK);
    assert_int_equal(http_headers_add(&response_headers, HTTP_HEADER_CONTENT_LENGTH, span_from_str_literal("14")), HL_RESULT_OK);
    http_response_t outgoing;
    assert_int_equal(http_response_initialize(&outgoing, HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200, response_headers), ok);
    outgoing.body = span_from_str_literal("via the tunnel");
    assert_int_equal(http_connection_send_response(&server_connection, &outgoing), ok);

    uint8_t client_buffer[1024];
    http_response_t incoming;
    assert_int_equal(http_connection_receive_response(&client.connection, span_from_memory(client_buffer), &incoming, NULL), ok);
    assert_body_equals(&incoming, "via the tunnel");

    test_client_close(&client);
    (void)http_connection_close(&server_connection);
    (void)http_endpoint_deinit(&server_endpoint);
    connect_proxy_stop(proxy);
}

static void client_reaches_a_server_through_a_connect_proxy(void** state)
{
    (void)state;
    connect_proxy_t proxy;
    connect_proxy_start(&proxy, PROXY_TEST_SERVER_PORT);

    http_endpoint_config_t configuration = plain_client_configuration(PROXY_TEST_SERVER_PORT);
    /* Resolved by the proxy, never by the client. */
    configuration.remote.hostname = span_from_str_literal("server.invalid");
    configuration.proxy.hostname  = span_from_str_literal("127.0.0.1");
    configuration.proxy.port      = proxy.port;

    proxied_exchange_with_an_http_c_server(&configuration, &proxy);

    char expected[96];
    (void)snprintf(expected, sizeof(expected), "CONNECT server.invalid:%d HTTP/1.1\r\n", PROXY_TEST_SERVER_PORT);
    assert_true(strncmp(proxy.request, expected, strlen(expected)) == 0);
    assert_null(strstr(proxy.request, "Proxy-Authorization"));
}

/* ------------------------------------------------------------------------- *
 * Proxy configuration helpers.
 * ------------------------------------------------------------------------- */

static void assert_span_text(span_t actual, const char* expected)
{
    assert_int_equal(span_get_size(actual), strlen(expected));
    assert_memory_equal(span_get_ptr(actual), expected, strlen(expected));
}

static void proxy_url_with_scheme_host_and_port(void** state)
{
    (void)state;
    uint8_t storage[256];
    proxy_host_config_t proxy;
    assert_int_equal(http_endpoint_proxy_parse_url(span_from_str_literal("http://proxy.example.com:3128"),
                                                   span_from_memory(storage), &proxy), ok);
    assert_span_text(proxy.hostname, "proxy.example.com");
    assert_int_equal(span_get_ptr(proxy.hostname)[span_get_size(proxy.hostname)], '\0');
    assert_int_equal(proxy.port, 3128);
    assert_true(span_is_empty(proxy.authorization));
}

static void proxy_url_defaults(void** state)
{
    (void)state;
    uint8_t storage[256];
    proxy_host_config_t proxy;

    /* No port: the http default. */
    assert_int_equal(http_endpoint_proxy_parse_url(span_from_str_literal("http://proxy.local"),
                                                   span_from_memory(storage), &proxy), ok);
    assert_span_text(proxy.hostname, "proxy.local");
    assert_int_equal(proxy.port, 80);

    /* No scheme: taken as http. Trailing slash, path and blanks ignored. */
    assert_int_equal(http_endpoint_proxy_parse_url(span_from_str_literal("  10.0.0.1:8080/  "),
                                                   span_from_memory(storage), &proxy), ok);
    assert_span_text(proxy.hostname, "10.0.0.1");
    assert_int_equal(proxy.port, 8080);

    assert_int_equal(http_endpoint_proxy_parse_url(span_from_str_literal("HTTP://Proxy:1/some/path?q#f"),
                                                   span_from_memory(storage), &proxy), ok);
    assert_span_text(proxy.hostname, "Proxy");
    assert_int_equal(proxy.port, 1);

    /* An empty port after the colon also means the default. */
    assert_int_equal(http_endpoint_proxy_parse_url(span_from_str_literal("http://proxy:"),
                                                   span_from_memory(storage), &proxy), ok);
    assert_int_equal(proxy.port, 80);
}

static void proxy_url_ipv6_literal(void** state)
{
    (void)state;
    uint8_t storage[256];
    proxy_host_config_t proxy;
    assert_int_equal(http_endpoint_proxy_parse_url(span_from_str_literal("http://[::1]:3128/"),
                                                   span_from_memory(storage), &proxy), ok);
    assert_span_text(proxy.hostname, "::1");
    assert_int_equal(proxy.port, 3128);

    assert_int_equal(http_endpoint_proxy_parse_url(span_from_str_literal("http://[fe80::1]"),
                                                   span_from_memory(storage), &proxy), ok);
    assert_span_text(proxy.hostname, "fe80::1");
    assert_int_equal(proxy.port, 80);
}

static void proxy_url_credentials_become_basic_authorization(void** state)
{
    (void)state;
    uint8_t storage[256];
    proxy_host_config_t proxy;

    assert_int_equal(http_endpoint_proxy_parse_url(span_from_str_literal("http://bob:s3cret@proxy:3128"),
                                                   span_from_memory(storage), &proxy), ok);
    assert_span_text(proxy.hostname, "proxy");
    assert_span_text(proxy.authorization, "Basic Ym9iOnMzY3JldA==");
    assert_int_equal(span_get_ptr(proxy.authorization)[span_get_size(proxy.authorization)], '\0');

    /* Percent-encoded '@' in the password; the last '@' ends the userinfo. */
    assert_int_equal(http_endpoint_proxy_parse_url(span_from_str_literal("http://user:pa%40ss@proxy"),
                                                   span_from_memory(storage), &proxy), ok);
    assert_span_text(proxy.authorization, "Basic dXNlcjpwYUBzcw==");

    /* User only: an empty password. */
    (void)memset(storage, 0, sizeof(storage));
    assert_int_equal(http_endpoint_proxy_parse_url(span_from_str_literal("alice@proxy:8080"),
                                                   span_from_memory(storage), &proxy), ok);
    assert_span_text(proxy.authorization, "Basic YWxpY2U6");
    assert_int_equal(proxy.port, 8080);

    /* The decoded credentials staged in storage are wiped afterwards. */
    for (uint32_t index = (uint32_t)(span_get_ptr(proxy.authorization) - storage) + span_get_size(proxy.authorization) + 1;
         index < sizeof(storage); index++)
    {
        assert_int_not_equal(storage[index], ':');
    }
}

static void proxy_url_rejects_invalid_values(void** state)
{
    (void)state;
    uint8_t storage[256];
    proxy_host_config_t proxy;
    const char* invalid[] = {
        "",
        "   ",
        "http://",
        "https://proxy:3128",     /* TLS to the proxy itself is not supported */
        "socks5://proxy:1080",
        "http://proxy:0",
        "http://proxy:65536",
        "http://proxy:12ab",
        "http://proxy:123456",
        "http://:3128",
        "http://user:pass@:3128",
        "http://::1:3128",        /* unbracketed IPv6 */
        "http://[::1",
        "http://[]:80",
        "http://[::1]x",
        "http://pro xy:80",
        "http://user:%zz@proxy",
    };

    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++)
    {
        proxy.port = 1234;
        result_t result = http_endpoint_proxy_parse_url(
            span_init((uint8_t*)(uintptr_t)invalid[index], (uint32_t)strlen(invalid[index])),
            span_from_memory(storage), &proxy);
        if (result != invalid_argument)
        {
            fail_msg("'%s' was not rejected", invalid[index]);
        }
        assert_true(span_is_empty(proxy.hostname));
        assert_int_equal(proxy.port, 0);
    }

    assert_int_equal(http_endpoint_proxy_parse_url(span_from_str_literal("http://proxy"),
                                                   span_from_memory(storage), NULL), invalid_argument);
}

static void proxy_url_reports_insufficient_storage(void** state)
{
    (void)state;
    uint8_t storage[16];
    proxy_host_config_t proxy;

    assert_int_equal(http_endpoint_proxy_parse_url(span_from_str_literal("http://a-rather-long-proxy-name:3128"),
                                                   span_from_memory(storage), &proxy), insufficient_size);
    assert_int_equal(http_endpoint_proxy_parse_url(span_from_str_literal("http://bob:s3cret@proxy:3128"),
                                                   span_from_memory(storage), &proxy), insufficient_size);
    assert_true(span_is_empty(proxy.hostname));
    assert_int_equal(http_endpoint_proxy_parse_url(span_from_str_literal("http://proxy:3128"),
                                                   SPAN_EMPTY, &proxy), insufficient_size);
}

static bool bypass(const char* no_proxy, const char* hostname)
{
    return http_endpoint_proxy_should_bypass(span_init((uint8_t*)(uintptr_t)no_proxy, (uint32_t)strlen(no_proxy)),
                                             span_init((uint8_t*)(uintptr_t)hostname, (uint32_t)strlen(hostname)));
}

static void proxy_bypass_matching(void** state)
{
    (void)state;
    assert_true(bypass("*", "anything.example"));
    assert_true(bypass("localhost,127.0.0.1", "localhost"));
    assert_true(bypass("localhost, 127.0.0.1", "127.0.0.1"));
    assert_true(bypass("example.com", "example.com"));
    assert_true(bypass("example.com", "api.example.com"));
    assert_true(bypass(".example.com", "example.com"));
    assert_true(bypass(".example.com", "deep.api.example.com"));
    assert_true(bypass("*.example.com", "api.example.com"));
    assert_true(bypass("EXAMPLE.com", "Api.Example.COM"));
    assert_true(bypass("example.com:443", "example.com"));
    assert_true(bypass("::1", "::1"));
    assert_true(bypass("[::1]", "[::1]"));
    assert_true(bypass(" ,, example.org ", "www.example.org"));
    assert_true(bypass("example.com", "example.com."));

    assert_false(bypass("", "example.com"));
    assert_false(bypass("example.com", "badexample.com"));
    assert_false(bypass("api.example.com", "example.com"));
    assert_false(bypass("localhost", "localhost.example"));
    assert_false(bypass("*.", "example.com"));
    assert_false(bypass("10.0.0.0/8", "10.1.2.3"));
    assert_false(bypass("example.com", ""));
}

/* Saves and clears every proxy variable, so the tests see only what they set
 * (the machine running them may well sit behind a proxy). */
static const char* const proxy_variable_names[] = {
    "http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "no_proxy", "NO_PROXY",
};
#define PROXY_VARIABLE_COUNT (sizeof(proxy_variable_names) / sizeof(proxy_variable_names[0]))

typedef struct saved_environment
{
    char* values[PROXY_VARIABLE_COUNT];
} saved_environment_t;

static int clear_proxy_environment(void** state)
{
    saved_environment_t* saved = (saved_environment_t*)calloc(1, sizeof(saved_environment_t));
    if (saved == NULL)
    {
        return -1;
    }
    for (size_t index = 0; index < PROXY_VARIABLE_COUNT; index++)
    {
        const char* value = getenv(proxy_variable_names[index]);
        saved->values[index] = value != NULL ? strdup(value) : NULL;
        (void)unsetenv(proxy_variable_names[index]);
    }
    *state = saved;
    return 0;
}

static int restore_proxy_environment(void** state)
{
    saved_environment_t* saved = (saved_environment_t*)*state;
    for (size_t index = 0; index < PROXY_VARIABLE_COUNT; index++)
    {
        if (saved->values[index] != NULL)
        {
            (void)setenv(proxy_variable_names[index], saved->values[index], 1);
            free(saved->values[index]);
        }
        else
        {
            (void)unsetenv(proxy_variable_names[index]);
        }
    }
    free(saved);
    return 0;
}

static http_endpoint_config_t client_configuration_for(const char* hostname, bool tls)
{
    http_endpoint_config_t configuration = http_endpoint_get_default_secure_client_config();
    configuration.tls.enable = tls;
    configuration.remote.hostname = span_init((uint8_t*)(uintptr_t)hostname, (uint32_t)strlen(hostname));
    configuration.remote.port = tls ? 443 : 80;
    return configuration;
}

static void proxy_environment_picks_the_variable_for_the_scheme(void** state)
{
    (void)state;
    uint8_t storage[256];
    (void)setenv("http_proxy", "http://plain-proxy:8080", 1);
    (void)setenv("HTTPS_PROXY", "http://secure-proxy:3128", 1);

    http_endpoint_config_t secure = client_configuration_for("data.example.org", true);
    assert_int_equal(http_endpoint_config_set_proxy_from_environment(&secure, span_from_memory(storage)), ok);
    assert_span_text(secure.proxy.hostname, "secure-proxy");
    assert_int_equal(secure.proxy.port, 3128);

    http_endpoint_config_t plain = client_configuration_for("data.example.org", false);
    assert_int_equal(http_endpoint_config_set_proxy_from_environment(&plain, span_from_memory(storage)), ok);
    assert_span_text(plain.proxy.hostname, "plain-proxy");
    assert_int_equal(plain.proxy.port, 8080);
}

static void proxy_environment_prefers_lowercase(void** state)
{
    (void)state;
    uint8_t storage[256];
    (void)setenv("https_proxy", "http://lower:1", 1);
    (void)setenv("HTTPS_PROXY", "http://upper:2", 1);

    http_endpoint_config_t configuration = client_configuration_for("example.com", true);
    assert_int_equal(http_endpoint_config_set_proxy_from_environment(&configuration, span_from_memory(storage)), ok);
    assert_span_text(configuration.proxy.hostname, "lower");

    /* An empty lowercase value falls through to the uppercase one. */
    (void)setenv("https_proxy", "", 1);
    assert_int_equal(http_endpoint_config_set_proxy_from_environment(&configuration, span_from_memory(storage)), ok);
    assert_span_text(configuration.proxy.hostname, "upper");
}

static void proxy_environment_without_a_proxy_clears_it(void** state)
{
    (void)state;
    uint8_t storage[256];
    http_endpoint_config_t configuration = client_configuration_for("example.com", true);
    configuration.proxy.hostname = span_from_str_literal("stale");
    configuration.proxy.port = 1;

    /* Only the plain-HTTP variable is set; a TLS client does not use it. */
    (void)setenv("http_proxy", "http://plain-proxy:8080", 1);

    assert_int_equal(http_endpoint_config_set_proxy_from_environment(&configuration, span_from_memory(storage)), ok);
    assert_true(span_is_empty(configuration.proxy.hostname));
    assert_int_equal(configuration.proxy.port, 0);
}

static void proxy_environment_honours_no_proxy(void** state)
{
    (void)state;
    uint8_t storage[256];
    (void)setenv("https_proxy", "http://proxy:3128", 1);
    (void)setenv("NO_PROXY", "localhost,.internal", 1);

    http_endpoint_config_t internal = client_configuration_for("db.internal", true);
    assert_int_equal(http_endpoint_config_set_proxy_from_environment(&internal, span_from_memory(storage)), ok);
    assert_true(span_is_empty(internal.proxy.hostname));

    http_endpoint_config_t external = client_configuration_for("www.example.org", true);
    assert_int_equal(http_endpoint_config_set_proxy_from_environment(&external, span_from_memory(storage)), ok);
    assert_span_text(external.proxy.hostname, "proxy");

    (void)setenv("no_proxy", "*", 1);
    assert_int_equal(http_endpoint_config_set_proxy_from_environment(&external, span_from_memory(storage)), ok);
    assert_true(span_is_empty(external.proxy.hostname));
}

static void proxy_environment_rejects_a_malformed_url(void** state)
{
    (void)state;
    uint8_t storage[256];
    (void)setenv("https_proxy", "https://proxy:3128", 1);

    http_endpoint_config_t configuration = client_configuration_for("example.com", true);
    assert_int_equal(http_endpoint_config_set_proxy_from_environment(&configuration, span_from_memory(storage)), invalid_argument);
    assert_true(span_is_empty(configuration.proxy.hostname));
    assert_int_equal(http_endpoint_config_set_proxy_from_environment(NULL, span_from_memory(storage)), invalid_argument);
}

/* The whole path: environment -> configuration -> CONNECT with credentials. */
static void client_reaches_a_server_through_a_proxy_from_the_environment(void** state)
{
    (void)state;
    connect_proxy_t proxy;
    connect_proxy_start(&proxy, PROXY_TEST_SERVER_PORT + 1);

    char proxy_url[64];
    (void)snprintf(proxy_url, sizeof(proxy_url), "http://user:pa%%40ss@127.0.0.1:%u", proxy.port);
    (void)setenv("http_proxy", proxy_url, 1);

    uint8_t storage[256];
    http_endpoint_config_t configuration = plain_client_configuration(PROXY_TEST_SERVER_PORT + 1);
    configuration.remote.hostname = span_from_str_literal("localhost");
    assert_int_equal(http_endpoint_config_set_proxy_from_environment(&configuration, span_from_memory(storage)), ok);
    assert_int_equal(configuration.proxy.port, proxy.port);

    /* The http-c server for this one listens on the port after the other. */
    http_endpoint_t server_endpoint;
    http_endpoint_config_t server_configuration = http_endpoint_get_default_secure_server_config();
    server_configuration.tls.enable = false;
    server_configuration.local.port = PROXY_TEST_SERVER_PORT + 1;
    assert_int_equal(http_endpoint_init(&server_endpoint, &server_configuration), ok);

    http_connection_t server_connection;
    task_t* accept_task = http_endpoint_wait_for_connection_async(&server_endpoint, &server_connection);
    assert_non_null(accept_task);

    test_client_t client;
    test_client_connect(&client, &configuration);
    assert_true(task_wait(accept_task));
    task_release(accept_task);

    test_client_close(&client);
    (void)http_connection_close(&server_connection);
    (void)http_endpoint_deinit(&server_endpoint);
    connect_proxy_stop(&proxy);

    char expected[96];
    (void)snprintf(expected, sizeof(expected), "CONNECT localhost:%d HTTP/1.1\r\n", PROXY_TEST_SERVER_PORT + 1);
    assert_true(strncmp(proxy.request, expected, strlen(expected)) == 0);
    assert_non_null(strstr(proxy.request, "\r\nProxy-Authorization: Basic dXNlcjpwYUBzcw==\r\n"));
}

int test_http_client()
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(client_reads_a_content_length_body),
        cmocka_unit_test(client_reads_a_chunked_body_with_extensions_and_trailers),
        cmocka_unit_test(client_reads_a_body_delimited_by_close),
        cmocka_unit_test(client_reads_no_body_for_204),
        cmocka_unit_test(client_reads_no_body_for_304),
        cmocka_unit_test(client_reads_no_body_for_a_head_request),
        cmocka_unit_test(client_reports_insufficient_size_for_an_oversized_close_delimited_body),
        cmocka_unit_test(client_reports_insufficient_size_for_an_oversized_content_length_body),
        cmocka_unit_test(client_reports_a_truncated_content_length_body),
        cmocka_unit_test(client_reports_a_malformed_chunk_size),
        cmocka_unit_test(client_streams_a_large_content_length_body),
        cmocka_unit_test(client_streams_a_large_chunked_body),
        cmocka_unit_test(client_streams_a_large_close_delimited_body),
        cmocka_unit_test(client_streaming_stops_when_the_handler_fails),
        cmocka_unit_test(client_streaming_a_bodiless_response_never_calls_the_handler),
        cmocka_unit_test(client_streaming_requires_a_handler),
        cmocka_unit_test(client_reaches_a_server_through_a_connect_proxy),
        cmocka_unit_test(proxy_url_with_scheme_host_and_port),
        cmocka_unit_test(proxy_url_defaults),
        cmocka_unit_test(proxy_url_ipv6_literal),
        cmocka_unit_test(proxy_url_credentials_become_basic_authorization),
        cmocka_unit_test(proxy_url_rejects_invalid_values),
        cmocka_unit_test(proxy_url_reports_insufficient_storage),
        cmocka_unit_test(proxy_bypass_matching),
        cmocka_unit_test_setup_teardown(proxy_environment_picks_the_variable_for_the_scheme,
                                        clear_proxy_environment, restore_proxy_environment),
        cmocka_unit_test_setup_teardown(proxy_environment_prefers_lowercase,
                                        clear_proxy_environment, restore_proxy_environment),
        cmocka_unit_test_setup_teardown(proxy_environment_without_a_proxy_clears_it,
                                        clear_proxy_environment, restore_proxy_environment),
        cmocka_unit_test_setup_teardown(proxy_environment_honours_no_proxy,
                                        clear_proxy_environment, restore_proxy_environment),
        cmocka_unit_test_setup_teardown(proxy_environment_rejects_a_malformed_url,
                                        clear_proxy_environment, restore_proxy_environment),
        cmocka_unit_test_setup_teardown(client_reaches_a_server_through_a_proxy_from_the_environment,
                                        clear_proxy_environment, restore_proxy_environment),
    };

    return cmocka_run_group_tests_name("http_client", tests, NULL, NULL);
}
