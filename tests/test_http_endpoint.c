#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <inttypes.h>

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <cmocka.h>

#include "niceties.h"

#include <http_request.h>
#include "http_endpoint.h"
#include "http_methods.h"
#include "http_versions.h"
#include "http_codes.h"

#include <test_http.h>

#define CLIENT_CERT_PATH "/tmp/http-c-certs/client/client.cert.pem"
#define CLIENT_PK_PATH "/tmp/http-c-certs/client/client.key.pem"
#define SERVER_CERT_PATH "/tmp/http-c-certs/server/server.cert.pem"
#define SERVER_PK_PATH "/tmp/http-c-certs/server/server.key.pem"
#define CA_CHAIN_PATH "/tmp/http-c-certs/ca/chain.ca.cert.pem"

static uint8_t TEST_HTTP_REQUEST_GET_1[] = "GET / HTTP/1.1\r\n\
Host: localhost:1234\r\n\
User-Agent: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:75.0) Gecko/20100101 Firefox/75.0\r\n\
Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8\r\n\
Accept-Language: en-US,en;q=0.5\r\n\
Accept-Encoding: gzip, deflate, br\r\n\
Connection: keep-alive\r\n\
Upgrade-Insecure-Requests: 1\r\n\
\r\n";

static uint8_t TEST_HTTP_RESPONSE_GET_1[] = "HTTP/1.1 200 OK\r\n\
Server: http-c\r\n\
Content-Type: text/html; charset=UTF-8\r\n\
Content-Length: 43\r\n\
\r\n";

static uint8_t test_raw_buffer[1024];

static span_t HTTP_HEADER_USER_AGENT_VALUE = span_from_str_literal("Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:75.0) Gecko/20100101 Firefox/75.0");
static span_t HTTP_HEADER_CONNECTION_VALUE = span_from_str_literal("keep-alive");

static span_t HTTP_HEADER_SERVER_VALUE = span_from_str_literal("http-c");
static span_t HTTP_HEADER_CONTENT_TYPE_VALUE = span_from_str_literal("text/html; charset=UTF-8");
static span_t HTTP_HEADER_CONTENT_LENGTH_VALUE = span_from_str_literal("43");
static span_t HTTP_RESPONSE_BODY = span_from_str_literal("<html><body>Hello from http-c</body></html>");

static void http_endpoint_init_listener_succeed(void** state)
{
    (void)state;
    http_endpoint_t local_endpoint;
    http_endpoint_config_t local_endpoint_config = http_endpoint_get_default_secure_server_config();
    local_endpoint_config.local.port = 4343;

    assert_int_equal(http_endpoint_init(&local_endpoint, &local_endpoint_config), ok);
    assert_int_equal(http_endpoint_deinit(&local_endpoint), ok);
}

static void http_endpoint_client_and_server_succeed(void** state)
{
    (void)state;
    http_endpoint_t server_endpoint;
    http_endpoint_config_t server_endpoint_config = http_endpoint_get_default_secure_server_config();
    server_endpoint_config.local.port = 4344;
    server_endpoint_config.tls.certificate_file = SERVER_CERT_PATH;
    server_endpoint_config.tls.private_key_file = SERVER_PK_PATH;

    http_endpoint_t client_endpoint;
    http_endpoint_config_t client_endpoint_config = http_endpoint_get_default_secure_client_config();
    client_endpoint_config.remote.hostname = span_from_str_literal("localhost");
    client_endpoint_config.remote.port = server_endpoint_config.local.port;
    client_endpoint_config.tls.certificate_file = CLIENT_CERT_PATH;
    client_endpoint_config.tls.private_key_file = CLIENT_PK_PATH;
    client_endpoint_config.tls.trusted_certificate_file = CA_CHAIN_PATH;

    assert_int_equal(http_endpoint_init(&server_endpoint, &server_endpoint_config), ok);

    assert_int_equal(http_endpoint_init(&client_endpoint, &client_endpoint_config), ok);

    http_connection_t server_connection;
    task_t* wait_for_connection_task = http_endpoint_wait_for_connection_async(&server_endpoint, &server_connection);

    http_connection_t client_connection;
    assert_int_equal(http_endpoint_connect(&client_endpoint, &client_connection), ok);

    assert_true(task_wait(wait_for_connection_task));
    assert_true(task_is_completed(wait_for_connection_task));
    assert_int_equal(task_get_result(wait_for_connection_task), ok);
    task_release(wait_for_connection_task);

    span_t test_buffer = span_from_memory(test_raw_buffer);

    http_headers_t outgoing_request_headers, outgoing_response_headers;
    http_request_t outgoing_request;
    http_request_t incoming_request;
    http_response_t outgoing_response;
    http_response_t incoming_response;
    span_t header_name, header_value;

    assert_int_equal(http_headers_init(&outgoing_request_headers, test_buffer), HL_RESULT_OK);
    assert_int_equal(http_headers_add(&outgoing_request_headers, HTTP_HEADER_USER_AGENT, HTTP_HEADER_USER_AGENT_VALUE), HL_RESULT_OK);
    assert_int_equal(http_headers_add(&outgoing_request_headers, HTTP_HEADER_CONNECTION, HTTP_HEADER_CONNECTION_VALUE), HL_RESULT_OK);
    assert_int_equal(http_request_initialize(&outgoing_request, HTTP_METHOD_GET, span_from_str_literal("/"), HTTP_VERSION_1_1, outgoing_request_headers), ok);
    assert_int_equal(http_connection_send_request(&client_connection, &outgoing_request), ok);

    assert_int_equal(http_connection_receive_request(&server_connection, test_buffer, &incoming_request, NULL), ok);
    assert_int_equal(span_compare(incoming_request.method, HTTP_METHOD_GET), 0);
    assert_int_equal(span_compare(incoming_request.path, span_from_str_literal("/")), 0);
    assert_int_equal(span_compare(incoming_request.http_version, HTTP_VERSION_1_1), 0);

    assert_int_equal(http_headers_get_next(&incoming_request.headers, &header_name, &header_value), HL_RESULT_OK);
    assert_int_equal(span_get_size(header_name), span_get_size(HTTP_HEADER_USER_AGENT)); 
    assert_memory_equal(span_get_ptr(header_name), span_get_ptr(HTTP_HEADER_USER_AGENT), span_get_size(HTTP_HEADER_USER_AGENT)); 
    assert_int_equal(span_get_size(header_value), span_get_size(HTTP_HEADER_USER_AGENT_VALUE)); 
    assert_memory_equal(span_get_ptr(header_value), span_get_ptr(HTTP_HEADER_USER_AGENT_VALUE), span_get_size(HTTP_HEADER_USER_AGENT_VALUE)); 
    assert_int_equal(http_headers_get_next(&incoming_request.headers, &header_name, &header_value), HL_RESULT_OK);
    assert_int_equal(span_get_size(header_name), span_get_size(HTTP_HEADER_CONNECTION)); 
    assert_memory_equal(span_get_ptr(header_name), span_get_ptr(HTTP_HEADER_CONNECTION), span_get_size(HTTP_HEADER_CONNECTION)); 
    assert_int_equal(span_get_size(header_value), span_get_size(HTTP_HEADER_CONNECTION_VALUE)); 
    assert_memory_equal(span_get_ptr(header_value), span_get_ptr(HTTP_HEADER_CONNECTION_VALUE), span_get_size(HTTP_HEADER_CONNECTION_VALUE));
    assert_int_equal(http_headers_get_next(&incoming_request.headers, &header_name, &header_value), HL_RESULT_EOF);

    assert_int_equal(http_headers_init(&outgoing_response_headers, test_buffer), HL_RESULT_OK);
    assert_int_equal(http_headers_add(&outgoing_response_headers, HTTP_HEADER_SERVER, HTTP_HEADER_SERVER_VALUE), HL_RESULT_OK);
    assert_int_equal(http_headers_add(&outgoing_response_headers, HTTP_HEADER_CONTENT_TYPE, HTTP_HEADER_CONTENT_TYPE_VALUE), HL_RESULT_OK);
    assert_int_equal(http_headers_add(&outgoing_response_headers, HTTP_HEADER_CONTENT_LENGTH, HTTP_HEADER_CONTENT_LENGTH_VALUE), HL_RESULT_OK);
    assert_int_equal(http_response_initialize(&outgoing_response, HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200, outgoing_response_headers), ok);
    outgoing_response.body = HTTP_RESPONSE_BODY;
    assert_int_equal(http_connection_send_response(&server_connection, &outgoing_response), ok);

    assert_int_equal(http_connection_receive_response(&client_connection, test_buffer, &incoming_response, NULL), ok);
    assert_int_equal(span_compare(incoming_response.http_version, HTTP_VERSION_1_1), 0);
    assert_int_equal(span_compare(incoming_response.code, HTTP_CODE_200), 0);
    assert_int_equal(span_compare(incoming_response.reason_phrase, HTTP_REASON_PHRASE_200), 0);

    assert_int_equal(http_headers_get_next(&incoming_response.headers, &header_name, &header_value), HL_RESULT_OK);
    assert_int_equal(span_compare(header_name, HTTP_HEADER_SERVER), 0);
    assert_int_equal(span_compare(header_value, HTTP_HEADER_SERVER_VALUE), 0);
    assert_int_equal(http_headers_get_next(&incoming_response.headers, &header_name, &header_value), HL_RESULT_OK);
    assert_int_equal(span_compare(header_name, HTTP_HEADER_CONTENT_TYPE), 0);
    assert_int_equal(span_compare(header_value, HTTP_HEADER_CONTENT_TYPE_VALUE), 0);
    assert_int_equal(http_headers_get_next(&incoming_response.headers, &header_name, &header_value), HL_RESULT_OK);
    assert_int_equal(span_compare(header_name, HTTP_HEADER_CONTENT_LENGTH), 0);
    assert_int_equal(span_compare(header_value, HTTP_HEADER_CONTENT_LENGTH_VALUE), 0);
    assert_int_equal(http_headers_get_next(&incoming_response.headers, &header_name, &header_value), HL_RESULT_EOF);

    assert_int_equal(http_connection_close(&client_connection), ok);
    assert_int_equal(http_connection_close(&server_connection), ok);
    assert_int_equal(http_endpoint_deinit(&client_endpoint), ok);
    assert_int_equal(http_endpoint_deinit(&server_endpoint), ok);
}

/* Restoring stdin has to be a fixture, not tail code: cmocka leaves a failed
 * test by longjmp, so any assertion below would skip it and strand every
 * later test without a descriptor 0. */
static int save_stdin(void** state)
{
    int saved = dup(STDIN_FILENO);
    if (saved < 0)
    {
        return -1;
    }
    *state = (void*)(intptr_t)saved;
    return 0;
}

static int restore_stdin(void** state)
{
    int saved = (int)(intptr_t)*state;
    (void)close(STDIN_FILENO);
    (void)dup2(saved, STDIN_FILENO);
    (void)close(saved);
    return 0;
}

/* A client endpoint owns no descriptor, so its teardown must close nothing.
 * It used to close fd 0 -- see http_endpoint_init. The defect is specific to
 * descriptor 0, so put a recognizable one there: open() takes the lowest free
 * number once stdin is closed. Plain TCP, so no TLS fixtures. */
static void http_endpoint_client_deinit_leaves_descriptor_zero_open(void** state)
{
    (void)state;

    assert_int_equal(close(STDIN_FILENO), 0);
    assert_int_equal(open("/dev/null", O_RDONLY), STDIN_FILENO);

    http_endpoint_t client_endpoint;
    http_endpoint_config_t client_endpoint_config = http_endpoint_get_default_secure_client_config();
    client_endpoint_config.remote.hostname = span_from_str_literal("localhost");
    client_endpoint_config.remote.port = 4345;
    client_endpoint_config.tls.enable = false;

    assert_int_equal(http_endpoint_init(&client_endpoint, &client_endpoint_config), ok);
    assert_int_equal(http_endpoint_deinit(&client_endpoint), ok);
    assert_int_not_equal(fcntl(STDIN_FILENO, F_GETFD), -1);
}

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* A peer that completes the handshake and then says nothing used to retry
 * try_again forever, spinning on sched_yield. A backlogged listener that is
 * never accepted reproduces it: the kernel finishes the client's connect, so
 * the request goes out and no reply ever comes. */
static void http_connection_read_gives_up_on_a_silent_peer(void** state)
{
    (void)state;

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert_true(listener >= 0);

    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0; /* any free port */
    assert_int_equal(bind(listener, (struct sockaddr*)&addr, sizeof(addr)), 0);
    assert_int_equal(listen(listener, 1), 0);

    socklen_t addr_len = sizeof(addr);
    assert_int_equal(getsockname(listener, (struct sockaddr*)&addr, &addr_len), 0);

    http_endpoint_t endpoint;
    http_endpoint_config_t config = http_endpoint_get_default_secure_client_config();
    config.tls.enable      = false;
    config.remote.hostname = span_from_str_literal("127.0.0.1");
    config.remote.port     = ntohs(addr.sin_port);
    config.io_timeout_ms   = 300;

    assert_int_equal(http_endpoint_init(&endpoint, &config), ok);

    http_connection_t connection;
    assert_int_equal(http_endpoint_connect(&endpoint, &connection), ok);

    uint8_t        header_storage[256];
    http_headers_t headers;
    (void)http_headers_init(&headers, span_init(header_storage, (uint32_t)sizeof(header_storage)));
    (void)http_headers_add(&headers, HTTP_HEADER_HOST, span_from_str_literal("127.0.0.1"));

    http_request_t request;
    assert_int_equal(http_request_initialize(&request, HTTP_METHOD_GET,
                                             span_from_str_literal("/"),
                                             HTTP_VERSION_1_1, headers), ok);
    assert_int_equal(http_connection_send_request(&connection, &request), ok);

    uint8_t         response_buffer[512];
    http_response_t response;

    uint64_t started  = monotonic_ms();
    result_t received = http_connection_receive_response(
        &connection,
        span_init(response_buffer, (uint32_t)sizeof(response_buffer)),
        &response, NULL);
    uint64_t elapsed = monotonic_ms() - started;

    (void)http_connection_close(&connection);
    (void)http_endpoint_deinit(&endpoint);
    (void)close(listener);

    assert_true(is_error(received));
    /* Generous upper bound -- the point is that it returns at all, and the
     * old code never would. The lower bound matters just as much: returning
     * instantly would mean the read failed for some unrelated reason rather
     * than by running out its budget. */
    assert_true(elapsed >= 300);
    assert_true(elapsed < 5000);
}

/* A peer that completes the TCP handshake and then says nothing is not a
 * hypothetical: an address whose route is black-holed behaves exactly like
 * this, and DNS hands those out alongside working ones. The TLS handshake
 * has no bound of its own, so before io_timeout_ms reached the socket layer
 * this call never returned -- and with one worker thread behind it, that is
 * a service that stops answering rather than a slow request.
 *
 * The listener here accepts and never writes, so the client's ClientHello
 * goes unanswered. No fixtures: the client's TLS never gets far enough to
 * need a certificate from anyone. */
static void http_endpoint_client_handshake_gives_up_on_a_silent_peer(void** state)
{
    (void)state;

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert_true(listener >= 0);

    int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(4347);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    assert_int_equal(bind(listener, (struct sockaddr*)&addr, sizeof(addr)), 0);
    assert_int_equal(listen(listener, 4), 0);

    http_endpoint_t endpoint;
    http_endpoint_config_t config = http_endpoint_get_default_secure_client_config();
    config.remote.hostname = span_from_str_literal("127.0.0.1");
    config.remote.port     = 4347;
    config.io_timeout_ms   = 500;

    assert_int_equal(http_endpoint_init(&endpoint, &config), ok);

    http_connection_t connection;
    uint64_t started = monotonic_ms();
    result_t result  = http_endpoint_connect(&endpoint, &connection);
    uint64_t elapsed = monotonic_ms() - started;

    assert_int_not_equal(result, ok);
    assert_true(elapsed >= 400);
    /* Generous, because the point is that it returns at all. */
    assert_true(elapsed < 15000);

    (void)http_endpoint_deinit(&endpoint);
    close(listener);
}

int test_http_endpoint()
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(http_endpoint_init_listener_succeed),
    cmocka_unit_test(http_endpoint_client_and_server_succeed),
    cmocka_unit_test_setup_teardown(http_endpoint_client_deinit_leaves_descriptor_zero_open,
                                    save_stdin, restore_stdin),
    cmocka_unit_test(http_connection_read_gives_up_on_a_silent_peer),
    cmocka_unit_test(http_endpoint_client_handshake_gives_up_on_a_silent_peer)
  };

  return cmocka_run_group_tests_name("http_endpoint", tests, NULL, NULL);
}
