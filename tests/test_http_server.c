#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <inttypes.h>
#include <string.h>

#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <cmocka.h>

#include "niceties.h"
#include "task.h"

#include <http_request.h>
#include <http_response.h>
#include "http_methods.h"
#include "http_codes.h"
#include "http_versions.h"
#include "http_headers.h"
#include "http_endpoint.h"
#include "http_connection.h"
#include "http_server.h"
#include "http_server_storage.h"

#include <test_http.h>

#define CLIENT_CERT_PATH "/tmp/http-c-certs/client/client.cert.pem"
#define CLIENT_PK_PATH "/tmp/http-c-certs/client/client.key.pem"
#define SERVER_CERT_PATH "/tmp/http-c-certs/server/server.cert.pem"
#define SERVER_PK_PATH "/tmp/http-c-certs/server/server.key.pem"
#define CA_CHAIN_PATH "/tmp/http-c-certs/ca/chain.ca.cert.pem"

/* Each test uses a distinct port to avoid TIME_WAIT collisions across runs. */
#define PORT_RUN_LIFECYCLE   4400
#define PORT_HANDLE_GET      4401
#define PORT_HANDLE_POST     4402
#define PORT_PATH_CAPTURES   4403
#define PORT_NOT_FOUND       4404
#define PORT_METHOD_MISMATCH 4405
#define PORT_KEEP_ALIVE      4406
#define PORT_PARALLEL        4407
#define PORT_PLAIN_HTTP      4408
#define PORT_STREAM_BODY     4409
#define PORT_DEFERRED_OK      4410
#define PORT_DEFERRED_TIMEOUT 4411
#define PORT_DEFERRED_STALE   4412
#define PORT_DEFERRED_BOGUS   4413
#define PORT_OVERSIZE         4414
#define PORT_CHUNKED          4415

/* ------------------------------------------------------------------------- *
 * Fixture helpers.
 * ------------------------------------------------------------------------- */

typedef struct test_client
{
    http_endpoint_t   endpoint;
    http_connection_t connection;
    uint8_t           buffer[2048];
} test_client_t;

static void server_set_default_tls(http_server_config_t* cfg, int port)
{
    *cfg = http_server_get_default_config();
    cfg->port = port;
    cfg->tls.certificate_file = SERVER_CERT_PATH;
    cfg->tls.private_key_file = SERVER_PK_PATH;
}

static void server_set_plain(http_server_config_t* cfg, int port)
{
    *cfg = http_server_get_default_config();
    cfg->port = port;
    cfg->tls.enable = false;
    cfg->tls.certificate_file = NULL;
    cfg->tls.private_key_file = NULL;
}

static void client_connect(test_client_t* client, int port)
{
    http_endpoint_config_t cfg = http_endpoint_get_default_secure_client_config();
    cfg.remote.hostname = span_from_str_literal("localhost");
    cfg.remote.port = port;
    cfg.tls.certificate_file = CLIENT_CERT_PATH;
    cfg.tls.private_key_file = CLIENT_PK_PATH;
    cfg.tls.trusted_certificate_file = CA_CHAIN_PATH;

    assert_int_equal(http_endpoint_init(&client->endpoint, &cfg), ok);
    assert_int_equal(http_endpoint_connect(&client->endpoint, &client->connection), ok);
}

static void client_connect_plain(test_client_t* client, int port)
{
    http_endpoint_config_t cfg = { 0 };
    cfg.role = http_endpoint_client;
    cfg.tls.enable = false;
    cfg.remote.hostname = span_from_str_literal("localhost");
    cfg.remote.port = port;

    assert_int_equal(http_endpoint_init(&client->endpoint, &cfg), ok);
    assert_int_equal(http_endpoint_connect(&client->endpoint, &client->connection), ok);
}

static void client_disconnect(test_client_t* client)
{
    (void)http_connection_close(&client->connection);
    (void)http_endpoint_deinit(&client->endpoint);
}

static span_t client_buffer(test_client_t* client)
{
    return span_from_memory(client->buffer);
}

static void send_simple_request(test_client_t* client,
                                span_t method,
                                span_t path,
                                span_t version,
                                span_t body /* may be SPAN_EMPTY */,
                                bool   close_after)
{
    static uint8_t hdr_storage[512];
    http_headers_t headers;
    assert_int_equal(http_headers_init(&headers, span_init(hdr_storage, sizeof(hdr_storage))), HL_RESULT_OK);
    assert_int_equal(http_headers_add(&headers, HTTP_HEADER_HOST, span_from_str_literal("localhost")), HL_RESULT_OK);

    if (close_after)
    {
        assert_int_equal(http_headers_add(&headers, HTTP_HEADER_CONNECTION, span_from_str_literal("close")), HL_RESULT_OK);
    }

    static uint8_t cl_storage[16];
    if (!span_is_empty(body))
    {
        span_t cl = span_copy_int32(span_init(cl_storage, sizeof(cl_storage)), (int32_t)span_get_size(body), NULL);
        assert_false(span_is_empty(cl));
        assert_int_equal(http_headers_add(&headers, HTTP_HEADER_CONTENT_LENGTH, cl), HL_RESULT_OK);
    }

    http_request_t request;
    assert_int_equal(http_request_initialize(&request, method, path, version, headers), ok);
    if (!span_is_empty(body))
    {
        request.body = body;
    }
    assert_int_equal(http_connection_send_request(&client->connection, &request), ok);
}

/* The responses the server writes for itself -- 404 and 405 -- have no
 * handler to give them a Content-Length, and a response carrying neither
 * that nor a Transfer-Encoding is delimited by the connection closing
 * (RFC 7230 3.3.3). Sent to a keep-alive client, one of those is a reply
 * the client cannot tell is over: `curl` against an unmatched route sat
 * waiting until it was interrupted. The header block was also terminated
 * twice, so two stray CRLF bytes led the body.
 *
 * Both are cheap to assert and neither was covered. */
static void assert_default_response_is_framed(http_response_t* response)
{
    span_t length;
    assert_int_equal(http_headers_find(&response->headers,
                                       HTTP_HEADER_CONTENT_LENGTH, &length),
                     HL_RESULT_OK);
    assert_int_equal(span_compare(length, span_from_str_literal("0")), 0);
    assert_true(span_is_empty(response->body));
}

/* Synchronization between handler invocation and the test driver. */
typedef struct handler_capture
{    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    bool            invoked;
    span_t          method;
    span_t          path;
    uint16_t        match_count;
    span_t          matches[5];
    uint8_t         match_storage[5][128];
    uint8_t         body_storage[256];
    uint32_t        body_size;
    /* Response programmed by the test. */
    span_t          response_code;
    span_t          response_reason;
    span_t          response_body;
    bool            count_invocations;
    int             invocation_count;
} handler_capture_t;

static void handler_capture_init(handler_capture_t* h)
{
    (void)memset(h, 0, sizeof(*h));
    (void)pthread_mutex_init(&h->mutex, NULL);
    (void)pthread_cond_init(&h->cond, NULL);
    h->response_code   = HTTP_CODE_200;
    h->response_reason = HTTP_REASON_PHRASE_200;
    h->response_body   = SPAN_EMPTY;
}

static void handler_capture_destroy(handler_capture_t* h)
{
    (void)pthread_cond_destroy(&h->cond);
    (void)pthread_mutex_destroy(&h->mutex);
}

static void capturing_handler(http_request_t* request,
                              span_t* path_matches,
                              uint16_t number_of_matches,
                              http_response_t* out_response,
                              void* user_context)
{
    handler_capture_t* h = (handler_capture_t*)user_context;

    (void)pthread_mutex_lock(&h->mutex);

    h->invoked = true;
    h->method  = request->method;
    h->path    = request->path;

    h->match_count = number_of_matches;
    for (uint16_t i = 0; i < number_of_matches && i < 5; i++)
    {
        uint32_t n = span_get_size(path_matches[i]);
        if (n > sizeof(h->match_storage[i])) n = sizeof(h->match_storage[i]);
        memcpy(h->match_storage[i], span_get_ptr(path_matches[i]), n);
        h->matches[i] = span_init(h->match_storage[i], n);
    }

    uint32_t bn = span_get_size(request->body);
    if (bn > sizeof(h->body_storage)) bn = sizeof(h->body_storage);
    if (bn > 0) memcpy(h->body_storage, span_get_ptr(request->body), bn);
    h->body_size = bn;

    out_response->code          = h->response_code;
    out_response->reason_phrase = h->response_reason;
    out_response->body          = h->response_body;

    if (h->count_invocations)
    {
        h->invocation_count++;
    }

    (void)pthread_cond_broadcast(&h->cond);
    (void)pthread_mutex_unlock(&h->mutex);
}

static void wait_for_handler(handler_capture_t* h, int max_ms)
{
    (void)pthread_mutex_lock(&h->mutex);
    while (!h->invoked && max_ms > 0)
    {
        (void)pthread_mutex_unlock(&h->mutex);
        task_sleep_ms(5);
        max_ms -= 5;
        (void)pthread_mutex_lock(&h->mutex);
    }
    (void)pthread_mutex_unlock(&h->mutex);
}

static void noop_handler(http_request_t* r, span_t* m, uint16_t n, http_response_t* o, void* u)
{
    (void)r; (void)m; (void)n; (void)o; (void)u;
}

/* ------------------------------------------------------------------------- *
 *                              Negative tests
 * ------------------------------------------------------------------------- */

static void http_server_init_NULL_server_fails(void** state)
{
    (void)state;
    http_server_config_t cfg = http_server_get_default_config();
    assert_int_equal(http_server_init(NULL, &cfg, http_server_storage_get_for_server_host()), invalid_argument);
}

static void http_server_init_NULL_config_fails(void** state)
{
    (void)state;
    http_server_t server;
    assert_int_equal(http_server_init(&server, NULL, http_server_storage_get_for_server_host()), invalid_argument);
}

static void http_server_init_NULL_storage_fails(void** state)
{
    (void)state;
    http_server_t server;
    http_server_config_t cfg = http_server_get_default_config();
    assert_int_equal(http_server_init(&server, &cfg, NULL), invalid_argument);
}

static void http_server_deinit_NULL_fails(void** state)
{
    (void)state;
    assert_int_equal(http_server_deinit(NULL), invalid_argument);
}

static void http_server_add_route_NULL_server_fails(void** state)
{
    (void)state;
    assert_int_equal(http_server_add_route(NULL, HTTP_METHOD_GET,
                                           span_from_str_literal("^/$"),
                                           noop_handler, NULL),
                     invalid_argument);
}

static void http_server_add_route_empty_method_fails(void** state)
{
    (void)state;
    http_server_t server;
    http_server_config_t cfg = http_server_get_default_config();
    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);

    assert_int_equal(http_server_add_route(&server, SPAN_EMPTY,
                                           span_from_str_literal("^/$"),
                                           noop_handler, NULL),
                     invalid_argument);

    assert_int_equal(http_server_deinit(&server), ok);
}

static void http_server_add_route_empty_path_fails(void** state)
{
    (void)state;
    http_server_t server;
    http_server_config_t cfg = http_server_get_default_config();
    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);

    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET, SPAN_EMPTY,
                                           noop_handler, NULL),
                     invalid_argument);

    assert_int_equal(http_server_deinit(&server), ok);
}

static void http_server_add_route_NULL_handler_fails(void** state)
{
    (void)state;
    http_server_t server;
    http_server_config_t cfg = http_server_get_default_config();
    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);

    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/$"),
                                           NULL, NULL),
                     invalid_argument);

    assert_int_equal(http_server_deinit(&server), ok);
}

static void http_server_add_route_overflow_fails(void** state)
{
    (void)state;
    http_server_t server;
    http_server_config_t cfg = http_server_get_default_config();
    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);

    for (uint32_t i = 0; i < server.storage->route_count; i++)
    {
        assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                               span_from_str_literal("^/$"),
                                               noop_handler, NULL),
                         ok);
    }

    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/$"),
                                           noop_handler, NULL),
                     insufficient_size);

    assert_int_equal(http_server_deinit(&server), ok);
}

static void http_server_run_NULL_fails(void** state)
{
    (void)state;
    assert_int_equal(http_server_run(NULL), invalid_argument);
}

static void http_server_stop_NULL_fails(void** state)
{
    (void)state;
    assert_int_equal(http_server_stop(NULL), invalid_argument);
}

/* ------------------------------------------------------------------------- *
 *                              Positive tests
 * ------------------------------------------------------------------------- */

static void http_server_init_and_deinit_succeed(void** state)
{
    (void)state;
    http_server_t server;
    http_server_config_t cfg = http_server_get_default_config();
    cfg.tls.certificate_file = SERVER_CERT_PATH;
    cfg.tls.private_key_file = SERVER_PK_PATH;
    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);
    assert_int_equal(http_server_deinit(&server), ok);
}

static void http_server_add_multiple_routes_succeed(void** state)
{
    (void)state;
    http_server_t server;
    http_server_config_t cfg = http_server_get_default_config();
    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);

    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/$"),
                                           noop_handler, NULL),
                     ok);
    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/index\\.html$"),
                                           noop_handler, NULL),
                     ok);
    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_POST,
                                           span_from_str_literal("^/api/.*$"),
                                           noop_handler, NULL),
                     ok);

    assert_int_equal(http_server_deinit(&server), ok);
}

static void http_server_run_lifecycle_succeed(void** state)
{
    (void)state;
    http_server_t server;
    http_server_config_t cfg;
    server_set_default_tls(&cfg, PORT_RUN_LIFECYCLE);

    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);
    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/$"),
                                           noop_handler, NULL), ok);

    task_t* run_task = http_server_run_async(&server);
    assert_non_null(run_task);

    /* Allow the run loop to enter the running state. */
    task_sleep_ms(50);

    assert_int_equal(http_server_stop(&server), ok);
    assert_true(task_wait(run_task));
    assert_true(task_is_completed(run_task));
    task_release(run_task);

    assert_int_equal(http_server_deinit(&server), ok);
}

static void http_server_handles_GET_request_succeed(void** state)
{
    (void)state;
    handler_capture_t handler;
    handler_capture_init(&handler);
    handler.response_body = span_from_str_literal("hello");

    http_server_t server;
    http_server_config_t cfg;
    server_set_default_tls(&cfg, PORT_HANDLE_GET);

    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);
    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/$"),
                                           capturing_handler, &handler), ok);

    task_t* run_task = http_server_run_async(&server);
    assert_non_null(run_task);
    task_sleep_ms(50);

    test_client_t client;
    client_connect(&client, PORT_HANDLE_GET);

    send_simple_request(&client, HTTP_METHOD_GET, span_from_str_literal("/"),
                        HTTP_VERSION_1_1, SPAN_EMPTY, true /* Connection: close */);

    http_response_t response;
    assert_int_equal(http_connection_receive_response(&client.connection,
                                                      client_buffer(&client),
                                                      &response, NULL),
                     ok);

    assert_int_equal(span_compare(response.http_version, HTTP_VERSION_1_1), 0);
    assert_int_equal(span_compare(response.code, HTTP_CODE_200), 0);
    assert_int_equal(span_compare(response.reason_phrase, HTTP_REASON_PHRASE_200), 0);

    wait_for_handler(&handler, 1000);
    assert_true(handler.invoked);
    assert_int_equal(span_compare(handler.method, HTTP_METHOD_GET), 0);
    assert_int_equal(span_compare(handler.path, span_from_str_literal("/")), 0);

    client_disconnect(&client);

    assert_int_equal(http_server_stop(&server), ok);
    assert_true(task_wait(run_task));
    task_release(run_task);
    assert_int_equal(http_server_deinit(&server), ok);
    handler_capture_destroy(&handler);
}

static void http_server_handles_POST_with_body_succeed(void** state)
{
    (void)state;
    handler_capture_t handler;
    handler_capture_init(&handler);
    handler.response_code   = HTTP_CODE_201;
    handler.response_reason = HTTP_REASON_PHRASE_201;

    http_server_t server;
    http_server_config_t cfg;
    server_set_default_tls(&cfg, PORT_HANDLE_POST);

    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);
    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_POST,
                                           span_from_str_literal("^/api/items$"),
                                           capturing_handler, &handler), ok);

    task_t* run_task = http_server_run_async(&server);
    assert_non_null(run_task);
    task_sleep_ms(50);

    test_client_t client;
    client_connect(&client, PORT_HANDLE_POST);

    span_t body = span_from_str_literal("{\"name\":\"item-1\"}");
    send_simple_request(&client, HTTP_METHOD_POST,
                        span_from_str_literal("/api/items"),
                        HTTP_VERSION_1_1, body, true);

    http_response_t response;
    assert_int_equal(http_connection_receive_response(&client.connection,
                                                      client_buffer(&client),
                                                      &response, NULL),
                     ok);
    assert_int_equal(span_compare(response.code, HTTP_CODE_201), 0);
    assert_int_equal(span_compare(response.reason_phrase, HTTP_REASON_PHRASE_201), 0);

    wait_for_handler(&handler, 1000);
    assert_true(handler.invoked);
    assert_int_equal(span_compare(handler.method, HTTP_METHOD_POST), 0);
    assert_int_equal(handler.body_size, span_get_size(body));
    assert_memory_equal(handler.body_storage, span_get_ptr(body), span_get_size(body));

    client_disconnect(&client);

    assert_int_equal(http_server_stop(&server), ok);
    assert_true(task_wait(run_task));
    task_release(run_task);
    assert_int_equal(http_server_deinit(&server), ok);
    handler_capture_destroy(&handler);
}

static void http_server_path_captures_succeed(void** state)
{
    (void)state;
    handler_capture_t handler;
    handler_capture_init(&handler);

    http_server_t server;
    http_server_config_t cfg;
    server_set_default_tls(&cfg, PORT_PATH_CAPTURES);

    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);
    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/cars/([^/]+)/([^/]+)$"),
                                           capturing_handler, &handler), ok);

    task_t* run_task = http_server_run_async(&server);
    assert_non_null(run_task);
    task_sleep_ms(50);

    test_client_t client;
    client_connect(&client, PORT_PATH_CAPTURES);

    send_simple_request(&client, HTTP_METHOD_GET,
                        span_from_str_literal("/cars/toyota/corolla"),
                        HTTP_VERSION_1_1, SPAN_EMPTY, true);

    http_response_t response;
    assert_int_equal(http_connection_receive_response(&client.connection,
                                                      client_buffer(&client),
                                                      &response, NULL),
                     ok);
    assert_int_equal(span_compare(response.code, HTTP_CODE_200), 0);

    wait_for_handler(&handler, 1000);
    assert_true(handler.invoked);
    /* match[0] = full match, match[1] = first group, match[2] = second group. */
    assert_int_equal(handler.match_count, 3);
    assert_int_equal(span_compare(handler.matches[1], span_from_str_literal("toyota")), 0);
    assert_int_equal(span_compare(handler.matches[2], span_from_str_literal("corolla")), 0);

    client_disconnect(&client);
    assert_int_equal(http_server_stop(&server), ok);
    assert_true(task_wait(run_task));
    task_release(run_task);
    assert_int_equal(http_server_deinit(&server), ok);
    handler_capture_destroy(&handler);
}

static void http_server_returns_404_for_unknown_path(void** state)
{
    (void)state;
    handler_capture_t handler;
    handler_capture_init(&handler);

    http_server_t server;
    http_server_config_t cfg;
    server_set_default_tls(&cfg, PORT_NOT_FOUND);

    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);
    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/exists$"),
                                           capturing_handler, &handler), ok);

    task_t* run_task = http_server_run_async(&server);
    assert_non_null(run_task);
    task_sleep_ms(50);

    test_client_t client;
    client_connect(&client, PORT_NOT_FOUND);

    send_simple_request(&client, HTTP_METHOD_GET,
                        span_from_str_literal("/does-not-exist"),
                        HTTP_VERSION_1_1, SPAN_EMPTY, true);

    http_response_t response;
    assert_int_equal(http_connection_receive_response(&client.connection,
                                                      client_buffer(&client),
                                                      &response, NULL),
                     ok);
    assert_int_equal(span_compare(response.code, HTTP_CODE_404), 0);
    assert_int_equal(span_compare(response.reason_phrase, HTTP_REASON_PHRASE_404), 0);
    assert_false(handler.invoked);
    assert_default_response_is_framed(&response);

    client_disconnect(&client);
    assert_int_equal(http_server_stop(&server), ok);
    assert_true(task_wait(run_task));
    task_release(run_task);
    assert_int_equal(http_server_deinit(&server), ok);
    handler_capture_destroy(&handler);
}

static void http_server_returns_405_for_method_mismatch(void** state)
{
    (void)state;
    handler_capture_t handler;
    handler_capture_init(&handler);

    http_server_t server;
    http_server_config_t cfg;
    server_set_default_tls(&cfg, PORT_METHOD_MISMATCH);

    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);
    /* Only register POST; client will GET. */
    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_POST,
                                           span_from_str_literal("^/$"),
                                           capturing_handler, &handler), ok);

    task_t* run_task = http_server_run_async(&server);
    assert_non_null(run_task);
    task_sleep_ms(50);

    test_client_t client;
    client_connect(&client, PORT_METHOD_MISMATCH);

    send_simple_request(&client, HTTP_METHOD_GET,
                        span_from_str_literal("/"),
                        HTTP_VERSION_1_1, SPAN_EMPTY, true);

    http_response_t response;
    assert_int_equal(http_connection_receive_response(&client.connection,
                                                      client_buffer(&client),
                                                      &response, NULL),
                     ok);
    assert_int_equal(span_compare(response.code, HTTP_CODE_405), 0);
    assert_int_equal(span_compare(response.reason_phrase, HTTP_REASON_PHRASE_405), 0);
    assert_default_response_is_framed(&response);
    assert_false(handler.invoked);

    client_disconnect(&client);
    assert_int_equal(http_server_stop(&server), ok);
    assert_true(task_wait(run_task));
    task_release(run_task);
    assert_int_equal(http_server_deinit(&server), ok);
    handler_capture_destroy(&handler);
}

static void http_server_keep_alive_multiple_requests_succeed(void** state)
{
    (void)state;
    handler_capture_t handler;
    handler_capture_init(&handler);
    handler.count_invocations = true;

    http_server_t server;
    http_server_config_t cfg;
    server_set_default_tls(&cfg, PORT_KEEP_ALIVE);

    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);
    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/$"),
                                           capturing_handler, &handler), ok);

    task_t* run_task = http_server_run_async(&server);
    assert_non_null(run_task);
    task_sleep_ms(50);

    test_client_t client;
    client_connect(&client, PORT_KEEP_ALIVE);

    /* Send three keep-alive requests on the same connection, then close. */
    for (int i = 0; i < 3; i++)
    {
        bool last = (i == 2);
        send_simple_request(&client, HTTP_METHOD_GET,
                            span_from_str_literal("/"),
                            HTTP_VERSION_1_1, SPAN_EMPTY,
                            last /* close on last */);

        http_response_t response;
        assert_int_equal(http_connection_receive_response(&client.connection,
                                                          client_buffer(&client),
                                                          &response, NULL),
                         ok);
        assert_int_equal(span_compare(response.code, HTTP_CODE_200), 0);
    }

    /* Allow the worker to drain. */
    task_sleep_ms(100);

    (void)pthread_mutex_lock(&handler.mutex);
    int count = handler.invocation_count;
    (void)pthread_mutex_unlock(&handler.mutex);
    assert_int_equal(count, 3);

    client_disconnect(&client);
    assert_int_equal(http_server_stop(&server), ok);
    assert_true(task_wait(run_task));
    task_release(run_task);
    assert_int_equal(http_server_deinit(&server), ok);
    handler_capture_destroy(&handler);
}

static void http_server_handles_parallel_clients_succeed(void** state)
{
    (void)state;
    handler_capture_t handler;
    handler_capture_init(&handler);
    handler.count_invocations = true;

    http_server_t server;
    http_server_config_t cfg;
    server_set_default_tls(&cfg, PORT_PARALLEL);

    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);
    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/$"),
                                           capturing_handler, &handler), ok);

    task_t* run_task = http_server_run_async(&server);
    assert_non_null(run_task);
    task_sleep_ms(50);

    enum { N_CLIENTS = 4 };
    test_client_t clients[N_CLIENTS];

    for (int i = 0; i < N_CLIENTS; i++)
    {
        client_connect(&clients[i], PORT_PARALLEL);
    }

    /* Issue every request before reading any response so connections overlap. */
    for (int i = 0; i < N_CLIENTS; i++)
    {
        send_simple_request(&clients[i], HTTP_METHOD_GET,
                            span_from_str_literal("/"),
                            HTTP_VERSION_1_1, SPAN_EMPTY, true);
    }

    for (int i = 0; i < N_CLIENTS; i++)
    {
        http_response_t response;
        assert_int_equal(http_connection_receive_response(&clients[i].connection,
                                                          client_buffer(&clients[i]),
                                                          &response, NULL),
                         ok);
        assert_int_equal(span_compare(response.code, HTTP_CODE_200), 0);
    }

    task_sleep_ms(100);

    (void)pthread_mutex_lock(&handler.mutex);
    int count = handler.invocation_count;
    (void)pthread_mutex_unlock(&handler.mutex);
    assert_int_equal(count, N_CLIENTS);

    for (int i = 0; i < N_CLIENTS; i++)
    {
        client_disconnect(&clients[i]);
    }

    assert_int_equal(http_server_stop(&server), ok);
    assert_true(task_wait(run_task));
    task_release(run_task);
    assert_int_equal(http_server_deinit(&server), ok);
    handler_capture_destroy(&handler);
}

/* ------------------------------------------------------------------------- *
 * Plain-HTTP (no TLS) end-to-end test. Verifies the server and client
 * negotiate a plaintext TCP connection and successfully exchange a
 * GET/200 round-trip when tls.enable=false.
 * ------------------------------------------------------------------------- */
static void http_server_plain_http_GET_request_succeed(void** state)
{
    (void)state;
    handler_capture_t handler;
    handler_capture_init(&handler);
    handler.response_body = span_from_str_literal("plain-hello");

    http_server_t server;
    http_server_config_t cfg;
    server_set_plain(&cfg, PORT_PLAIN_HTTP);

    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);
    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/$"),
                                           capturing_handler, &handler), ok);

    task_t* run_task = http_server_run_async(&server);
    assert_non_null(run_task);
    task_sleep_ms(50);

    test_client_t client;
    client_connect_plain(&client, PORT_PLAIN_HTTP);

    send_simple_request(&client, HTTP_METHOD_GET, span_from_str_literal("/"),
                        HTTP_VERSION_1_1, SPAN_EMPTY, true /* Connection: close */);

    http_response_t response;
    assert_int_equal(http_connection_receive_response(&client.connection,
                                                      client_buffer(&client),
                                                      &response, NULL),
                     ok);

    assert_int_equal(span_compare(response.code, HTTP_CODE_200), 0);

    wait_for_handler(&handler, 1000);
    assert_true(handler.invoked);
    assert_int_equal(span_compare(handler.method, HTTP_METHOD_GET), 0);

    client_disconnect(&client);

    assert_int_equal(http_server_stop(&server), ok);
    assert_true(task_wait(run_task));
    task_release(run_task);
    assert_int_equal(http_server_deinit(&server), ok);
    handler_capture_destroy(&handler);
}

/* ------------------------------------------------------------------------- *
 * Streaming response body (http_body_provider_t). A handler streams a payload
 * larger than the send buffer; the server must pull it in multiple chunks and
 * invoke the finalizer exactly once.
 * ------------------------------------------------------------------------- */
#define STREAM_BODY_SIZE 20000u

typedef struct stream_ctx
{
    const uint8_t* payload;
    uint32_t       size;
    uint32_t       offset;
    uint32_t       provider_calls;   /* calls that returned > 0 */
    uint32_t       bytes_sent;       /* total bytes handed to the server */
    uint32_t       finalize_calls;
    uint8_t        hdr_buf[64];
    uint8_t        clen[16];
} stream_ctx_t;

static uint8_t s_stream_payload[STREAM_BODY_SIZE];

static uint32_t stream_provider(void* context, uint8_t* buffer, uint32_t buffer_size)
{
    stream_ctx_t* c = (stream_ctx_t*)context;
    uint32_t remaining = c->size - c->offset;
    if (remaining == 0) return 0;                       /* true EOF */
    uint32_t n = (remaining < buffer_size) ? remaining : buffer_size;
    memcpy(buffer, c->payload + c->offset, n);
    c->offset     += n;
    c->bytes_sent += n;
    c->provider_calls++;
    return n;
}

static void stream_finalizer(void* context)
{
    stream_ctx_t* c = (stream_ctx_t*)context;
    c->finalize_calls++;
}

static void streaming_handler(http_request_t* request,
                              span_t* path_matches,
                              uint16_t number_of_matches,
                              http_response_t* out_response,
                              void* user_context)
{
    (void)request; (void)path_matches; (void)number_of_matches;
    stream_ctx_t* c = (stream_ctx_t*)user_context;

    /* The streaming contract requires the handler to set Content-Length. */
    (void)http_headers_init(&out_response->headers,
                            span_init(c->hdr_buf, sizeof(c->hdr_buf)));
    span_t cl = span_copy_int32(span_init(c->clen, sizeof(c->clen)),
                                (int32_t)c->size, NULL);
    (void)http_headers_add(&out_response->headers, HTTP_HEADER_CONTENT_LENGTH, cl);

    out_response->code                  = HTTP_CODE_200;
    out_response->reason_phrase         = HTTP_REASON_PHRASE_200;
    out_response->body_provider         = stream_provider;
    out_response->body_finalizer        = stream_finalizer;
    out_response->body_provider_context = c;
}

static void http_server_streams_large_body_succeed(void** state)
{
    (void)state;

    for (uint32_t i = 0; i < STREAM_BODY_SIZE; i++)
    {
        s_stream_payload[i] = (uint8_t)('A' + (i % 26));
    }

    stream_ctx_t ctx;
    (void)memset(&ctx, 0, sizeof(ctx));
    ctx.payload = s_stream_payload;
    ctx.size    = STREAM_BODY_SIZE;

    http_server_t server;
    http_server_config_t cfg;
    server_set_plain(&cfg, PORT_STREAM_BODY);

    assert_int_equal(http_server_init(&server, &cfg, http_server_storage_get_for_server_host()), ok);
    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/stream$"),
                                           streaming_handler, &ctx), ok);

    task_t* run_task = http_server_run_async(&server);
    assert_non_null(run_task);
    task_sleep_ms(50);

    test_client_t client;
    client_connect_plain(&client, PORT_STREAM_BODY);

    send_simple_request(&client, HTTP_METHOD_GET, span_from_str_literal("/stream"),
                        HTTP_VERSION_1_1, SPAN_EMPTY, true /* Connection: close */);

    /* The shared 2 KB client buffer is too small for a 20 KB body — use a
     * local receive buffer big enough for the head + streamed body. */
    static uint8_t recv_buf[STREAM_BODY_SIZE + 512];
    http_response_t response;
    assert_int_equal(http_connection_receive_response(&client.connection,
                                                      span_init(recv_buf, sizeof(recv_buf)),
                                                      &response, NULL),
                     ok);

    assert_int_equal(span_compare(response.code, HTTP_CODE_200), 0);
    assert_int_equal(span_get_size(response.body), STREAM_BODY_SIZE);
    assert_memory_equal(span_get_ptr(response.body), s_stream_payload, STREAM_BODY_SIZE);

    client_disconnect(&client);

    assert_int_equal(http_server_stop(&server), ok);
    assert_true(task_wait(run_task));
    task_release(run_task);
    assert_int_equal(http_server_deinit(&server), ok);

    /* Delivered in multiple chunks (20 KB over an 8 KB send buffer) and the
     * finalizer ran exactly once. */
    assert_int_equal(ctx.bytes_sent, STREAM_BODY_SIZE);
    assert_true(ctx.provider_calls >= 3);
    assert_int_equal(ctx.finalize_calls, 1);
}

/* ------------------------------------------------------------------------- *
 * Deferred responses (http_response_defer / http_server_respond).
 *
 * The server is a single-threaded event loop, so before deferral a handler
 * that needed to wait had to block -- stalling every other connection.
 * These cover the mechanism plus the failure modes that would otherwise
 * surface as rare corruption: completing a handle whose slot has been
 * recycled, and a deferral nobody ever completes.
 * ------------------------------------------------------------------------- */

typedef struct deferred_ctx
{
    http_server_t*         server;
    pthread_mutex_t        mutex;
    bool                   deferred;
    bool                   release;
    result_t               defer_result;
    http_response_handle_t handle;
    result_t               respond_result;
    uint8_t                header_storage[256];
    uint8_t                length_storage[16];
    http_headers_t         headers;
} deferred_ctx_t;

static void deferred_ctx_init(deferred_ctx_t* ctx, http_server_t* server)
{
    (void)memset(ctx, 0, sizeof(*ctx));
    ctx->server         = server;
    ctx->defer_result   = error;
    ctx->respond_result = error;
    assert_int_equal(pthread_mutex_init(&ctx->mutex, NULL), 0);
}

static void deferred_ctx_destroy(deferred_ctx_t* ctx)
{
    (void)pthread_mutex_destroy(&ctx->mutex);
}

/* Runs on the server's loop thread. Deliberately free of cmocka asserts:
 * a failing assert longjmps, and doing that off the test's own thread
 * wrecks the run. Results are recorded and asserted by the test body. */
static void deferring_handler(http_request_t* request, span_t* matches,
                              uint16_t match_count, http_response_t* out_response,
                              void* user)
{
    (void)request; (void)matches; (void)match_count;
    deferred_ctx_t* ctx = (deferred_ctx_t*)user;

    http_response_handle_t handle;
    result_t r = http_response_defer(out_response, &handle);

    (void)pthread_mutex_lock(&ctx->mutex);
    ctx->defer_result = r;
    ctx->handle       = handle;
    ctx->deferred     = true;
    (void)pthread_mutex_unlock(&ctx->mutex);
}

static bool wait_for_deferral(deferred_ctx_t* ctx, int max_ms)
{
    for (int waited = 0; waited < max_ms; waited += 5)
    {
        bool ready;
        (void)pthread_mutex_lock(&ctx->mutex);
        ready = ctx->deferred;
        (void)pthread_mutex_unlock(&ctx->mutex);
        if (ready)
        {
            return true;
        }
        task_sleep_ms(5);
    }
    return false;
}

static bool wait_for_release(deferred_ctx_t* ctx, int max_ms)
{
    for (int waited = 0; waited < max_ms; waited += 5)
    {
        bool go;
        (void)pthread_mutex_lock(&ctx->mutex);
        go = ctx->release;
        (void)pthread_mutex_unlock(&ctx->mutex);
        if (go)
        {
            return true;
        }
        task_sleep_ms(5);
    }
    return false;
}

static const span_t DEFERRED_BODY = span_from_str_literal("deferred-hello");

static result_t deferred_responder(void* state, task_t* self)
{
    (void)self;
    deferred_ctx_t* ctx = (deferred_ctx_t*)state;

    if (!wait_for_deferral(ctx, 2000))
    {
        return error;
    }

    /* Hold the answer until the test explicitly releases us. That is what
     * makes the "loop still serves others" assertion airtight: when the
     * second client is served, this request is provably still
     * unanswered. */
    if (!wait_for_release(ctx, 5000))
    {
        return error;
    }

    http_response_t response;
    (void)memset(&response, 0, sizeof(response));
    response.http_version  = HTTP_VERSION_1_1;
    response.code          = HTTP_CODE_200;
    response.reason_phrase = HTTP_REASON_PHRASE_200;

    /* Header and body memory lives in `ctx` (owned by the test frame) and
     * in static storage, so both stay valid until the response is sent --
     * which is the contract http_server_respond documents. */
    (void)http_headers_init(&ctx->headers,
                            span_init(ctx->header_storage, sizeof(ctx->header_storage)));
    span_t cl = span_copy_int32(span_init(ctx->length_storage, sizeof(ctx->length_storage)),
                                (int32_t)span_get_size(DEFERRED_BODY), NULL);
    (void)http_headers_add(&ctx->headers, HTTP_HEADER_CONTENT_LENGTH, cl);
    response.headers = ctx->headers;
    response.body    = DEFERRED_BODY;

    result_t r = http_server_respond(ctx->server, ctx->handle, &response);

    (void)pthread_mutex_lock(&ctx->mutex);
    ctx->respond_result = r;
    (void)pthread_mutex_unlock(&ctx->mutex);
    return r;
}

static void http_response_defer_outside_handler_fails(void** state)
{
    (void)state;
    http_response_t response;
    (void)memset(&response, 0, sizeof(response));
    http_response_handle_t handle;

    assert_int_equal(http_response_defer(NULL, &handle), invalid_argument);
    assert_int_equal(http_response_defer(&response, NULL), invalid_argument);
    /* Nothing populated `deferral`, so there is no request to defer. */
    assert_int_equal(http_response_defer(&response, &handle), not_found);
    assert_false(response.deferred);
}

static void http_server_respond_rejects_invalid_handles(void** state)
{
    (void)state;

    http_server_t server;
    http_server_config_t cfg;
    server_set_plain(&cfg, PORT_DEFERRED_BOGUS);
    assert_int_equal(http_server_init(&server, &cfg,
                                      http_server_storage_get_for_server_host()), ok);

    http_response_t response;
    (void)memset(&response, 0, sizeof(response));
    response.http_version  = HTTP_VERSION_1_1;
    response.code          = HTTP_CODE_200;
    response.reason_phrase = HTTP_REASON_PHRASE_200;

    http_response_handle_t handle;
    (void)memset(&handle, 0, sizeof(handle));

    assert_int_equal(http_server_respond(NULL, handle, &response), invalid_argument);
    assert_int_equal(http_server_respond(&server, handle, NULL), invalid_argument);
    assert_int_equal(http_server_respond(&server, handle, &response), invalid_argument);

    /* A slot pointer outside this server's storage must be rejected on
     * bounds rather than dereferenced into a wild write. */
    uint8_t elsewhere = 0;
    handle.slot       = &elsewhere;
    handle.generation = 0;
    assert_int_equal(http_server_respond(&server, handle, &response), invalid_argument);

    assert_int_equal(http_server_deinit(&server), ok);
}

static void http_server_deferred_response_from_worker_thread_succeed(void** state)
{
    (void)state;

    http_server_t server;
    http_server_config_t cfg;
    server_set_plain(&cfg, PORT_DEFERRED_OK);

    assert_int_equal(http_server_init(&server, &cfg,
                                      http_server_storage_get_for_server_host()), ok);

    deferred_ctx_t ctx;
    deferred_ctx_init(&ctx, &server);

    handler_capture_t immediate;
    handler_capture_init(&immediate);
    immediate.response_body = span_from_str_literal("immediate");

    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/slow$"),
                                           deferring_handler, &ctx), ok);
    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/fast$"),
                                           capturing_handler, &immediate), ok);

    task_t* run_task = http_server_run_async(&server);
    assert_non_null(run_task);
    task_sleep_ms(50);

    task_t* responder = task_run(deferred_responder, &ctx);
    assert_non_null(responder);

    test_client_t slow_client;
    client_connect_plain(&slow_client, PORT_DEFERRED_OK);
    send_simple_request(&slow_client, HTTP_METHOD_GET, span_from_str_literal("/slow"),
                        HTTP_VERSION_1_1, SPAN_EMPTY, true);

    assert_true(wait_for_deferral(&ctx, 2000));

    /* The entire point of the feature: with one request parked, the loop
     * still serves everyone else. This request is issued after the
     * deferral and must complete before it. */
    test_client_t fast_client;
    client_connect_plain(&fast_client, PORT_DEFERRED_OK);
    send_simple_request(&fast_client, HTTP_METHOD_GET, span_from_str_literal("/fast"),
                        HTTP_VERSION_1_1, SPAN_EMPTY, true);

    http_response_t fast_response;
    assert_int_equal(http_connection_receive_response(&fast_client.connection,
                                                      client_buffer(&fast_client),
                                                      &fast_response, NULL), ok);
    assert_int_equal(span_compare(fast_response.code, HTTP_CODE_200), 0);
    assert_true(immediate.invoked);
    /* No body assertion here: capturing_handler sets no Content-Length, so
     * that response is connection-delimited and the client reads no body.
     * The deferred response below does set one, and is checked. */

    /* Only now let the worker answer the parked request. Everything above
     * therefore happened while /slow was still outstanding. */
    (void)pthread_mutex_lock(&ctx.mutex);
    ctx.release = true;
    (void)pthread_mutex_unlock(&ctx.mutex);

    /* And now the deferred one lands, from a different thread. */
    http_response_t slow_response;
    assert_int_equal(http_connection_receive_response(&slow_client.connection,
                                                      client_buffer(&slow_client),
                                                      &slow_response, NULL), ok);
    assert_int_equal(span_compare(slow_response.code, HTTP_CODE_200), 0);
    assert_int_equal(span_compare(slow_response.body, DEFERRED_BODY), 0);

    assert_true(task_wait(responder));
    task_release(responder);

    assert_int_equal(ctx.defer_result, ok);
    assert_int_equal(ctx.respond_result, ok);

    client_disconnect(&fast_client);
    client_disconnect(&slow_client);

    assert_int_equal(http_server_stop(&server), ok);
    assert_true(task_wait(run_task));
    task_release(run_task);
    assert_int_equal(http_server_deinit(&server), ok);
    handler_capture_destroy(&immediate);
    deferred_ctx_destroy(&ctx);
}

static void http_server_deferred_response_times_out_with_504(void** state)
{
    (void)state;

    http_server_t server;
    http_server_config_t cfg;
    server_set_plain(&cfg, PORT_DEFERRED_TIMEOUT);
    /* Short ceiling so the test does not sit out the 30s default. */
    cfg.deferred_timeout_ms = 200;

    assert_int_equal(http_server_init(&server, &cfg,
                                      http_server_storage_get_for_server_host()), ok);

    deferred_ctx_t ctx;
    deferred_ctx_init(&ctx, &server);

    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/never$"),
                                           deferring_handler, &ctx), ok);

    task_t* run_task = http_server_run_async(&server);
    assert_non_null(run_task);
    task_sleep_ms(50);

    test_client_t client;
    client_connect_plain(&client, PORT_DEFERRED_TIMEOUT);
    send_simple_request(&client, HTTP_METHOD_GET, span_from_str_literal("/never"),
                        HTTP_VERSION_1_1, SPAN_EMPTY, true);

    /* Nobody ever calls http_server_respond. Without the deadline sweep
     * this connection would be stranded for the life of the process. */
    http_response_t response;
    assert_int_equal(http_connection_receive_response(&client.connection,
                                                      client_buffer(&client),
                                                      &response, NULL), ok);
    assert_int_equal(span_compare(response.code, HTTP_CODE_504), 0);
    assert_default_response_is_framed(&response);

    assert_true(wait_for_deferral(&ctx, 1000));
    assert_int_equal(ctx.defer_result, ok);

    /* The handle died with the deferral. A late completion must be
     * refused, not written into whatever reuses the slot. */
    http_response_t late;
    (void)memset(&late, 0, sizeof(late));
    late.http_version  = HTTP_VERSION_1_1;
    late.code          = HTTP_CODE_200;
    late.reason_phrase = HTTP_REASON_PHRASE_200;
    assert_int_equal(http_server_respond(&server, ctx.handle, &late), not_found);

    client_disconnect(&client);
    assert_int_equal(http_server_stop(&server), ok);
    assert_true(task_wait(run_task));
    task_release(run_task);
    assert_int_equal(http_server_deinit(&server), ok);
    deferred_ctx_destroy(&ctx);
}

static void http_server_deferred_handle_stale_after_disconnect(void** state)
{
    (void)state;

    http_server_t server;
    http_server_config_t cfg;
    server_set_plain(&cfg, PORT_DEFERRED_STALE);
    /* Long ceiling: the disconnect must be what invalidates the handle,
     * not the sweep. */
    cfg.deferred_timeout_ms = 30000;

    assert_int_equal(http_server_init(&server, &cfg,
                                      http_server_storage_get_for_server_host()), ok);

    deferred_ctx_t ctx;
    deferred_ctx_init(&ctx, &server);

    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_GET,
                                           span_from_str_literal("^/abandon$"),
                                           deferring_handler, &ctx), ok);

    task_t* run_task = http_server_run_async(&server);
    assert_non_null(run_task);
    task_sleep_ms(50);

    test_client_t client;
    client_connect_plain(&client, PORT_DEFERRED_STALE);
    send_simple_request(&client, HTTP_METHOD_GET, span_from_str_literal("/abandon"),
                        HTTP_VERSION_1_1, SPAN_EMPTY, true);

    assert_true(wait_for_deferral(&ctx, 2000));

    /* The closed-tab case: the peer goes away while its request is
     * parked. The slot must be reclaimed promptly rather than held to
     * the deadline, and the outstanding handle must go stale with it. */
    client_disconnect(&client);
    task_sleep_ms(250);

    http_response_t late;
    (void)memset(&late, 0, sizeof(late));
    late.http_version  = HTTP_VERSION_1_1;
    late.code          = HTTP_CODE_200;
    late.reason_phrase = HTTP_REASON_PHRASE_200;
    assert_int_equal(http_server_respond(&server, ctx.handle, &late), not_found);

    assert_int_equal(http_server_stop(&server), ok);
    assert_true(task_wait(run_task));
    task_release(run_task);
    assert_int_equal(http_server_deinit(&server), ok);
    deferred_ctx_destroy(&ctx);
}

#define OVERSIZE_BODY_SIZE 10000u
static uint8_t s_oversize_body[OVERSIZE_BODY_SIZE];

static void http_server_oversize_request_returns_413(void** state)
{
    (void)state;

    /* The server answers then closes while the client may still be
     * writing, so a plain write() into the dying socket would raise
     * SIGPIPE and take the whole test binary down. */
    (void)signal(SIGPIPE, SIG_IGN);

    handler_capture_t handler;
    handler_capture_init(&handler);
    handler.response_body = span_from_str_literal("never");

    http_server_t server;
    http_server_config_t cfg;
    server_set_plain(&cfg, PORT_OVERSIZE);

    assert_int_equal(http_server_init(&server, &cfg,
                                      http_server_storage_get_for_server_host()), ok);
    assert_int_equal(http_server_add_route(&server, HTTP_METHOD_POST,
                                           span_from_str_literal("^/big$"),
                                           capturing_handler, &handler), ok);

    task_t* run_task = http_server_run_async(&server);
    assert_non_null(run_task);
    task_sleep_ms(50);

    (void)memset(s_oversize_body, 'x', sizeof(s_oversize_body));

    test_client_t client;
    client_connect_plain(&client, PORT_OVERSIZE);

    /* Bigger than the host storage's 8 KiB receive buffer. This used to
     * drop the connection with no status line at all, which a browser
     * reports as an unexplained network error with nothing to diagnose. */
    send_simple_request(&client, HTTP_METHOD_POST, span_from_str_literal("/big"),
                        HTTP_VERSION_1_1,
                        span_init(s_oversize_body, sizeof(s_oversize_body)), true);

    http_response_t response;
    assert_int_equal(http_connection_receive_response(&client.connection,
                                                      client_buffer(&client),
                                                      &response, NULL), ok);
    assert_int_equal(span_compare(response.code, HTTP_CODE_413), 0);
    assert_false(handler.invoked);

    client_disconnect(&client);
    assert_int_equal(http_server_stop(&server), ok);
    assert_true(task_wait(run_task));
    task_release(run_task);
    assert_int_equal(http_server_deinit(&server), ok);
    handler_capture_destroy(&handler);
}

/* ------------------------------------------------------------------------- *
 * Chunked response decoding (client side).
 *
 * http-c's own server always frames with Content-Length, so nothing in the
 * suite reached read_chunked_body -- yet GitHub, Docker and most real APIs
 * reply chunked, so every outbound call depends on it. The decoder also
 * dechunks in place, sharing a single buffer between the raw stream and
 * the decoded output, which is only sound because the output trails the
 * parse cursor. A hand-written server is the only way to pin that down.
 * ------------------------------------------------------------------------- */

static const char CHUNKED_RESPONSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "5\r\nHello\r\n"
    "1\r\n \r\n"
    "6\r\nchunky\r\n"
    "1\r\n \r\n"
    "5\r\nworld\r\n"
    "1\r\n!\r\n"
    "0\r\n\r\n";

#define CHUNKED_EXPECTED "Hello chunky world!"

typedef struct raw_server_ctx
{
    int             port;
    bool            listening;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
} raw_server_ctx_t;

/* volatile would not be enough: it orders nothing between threads and
 * carries no memory barrier, so the reader could observe the flag without
 * the writes that preceded it -- or never observe it at all. */
static void raw_server_signal_listening(raw_server_ctx_t* ctx)
{
    (void)pthread_mutex_lock(&ctx->mutex);
    ctx->listening = true;
    (void)pthread_cond_broadcast(&ctx->cond);
    (void)pthread_mutex_unlock(&ctx->mutex);
}

static bool raw_server_wait_listening(raw_server_ctx_t* ctx, int max_ms)
{
    bool listening = false;

    (void)pthread_mutex_lock(&ctx->mutex);
    for (int waited = 0; waited < max_ms && !ctx->listening; waited += 5)
    {
        (void)pthread_mutex_unlock(&ctx->mutex);
        task_sleep_ms(5);
        (void)pthread_mutex_lock(&ctx->mutex);
    }
    listening = ctx->listening;
    (void)pthread_mutex_unlock(&ctx->mutex);

    return listening;
}

static result_t raw_chunked_server(void* state, task_t* self)
{
    (void)self;
    raw_server_ctx_t* ctx = (raw_server_ctx_t*)state;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        return error;
    }

    int one = 1;
    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)ctx->port);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 1) != 0)
    {
        (void)close(listen_fd);
        return error;
    }

    raw_server_signal_listening(ctx);

    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
    {
        (void)close(listen_fd);
        return error;
    }

    /* Read the whole request head before answering.
     *
     * Closing a socket that still has unread bytes buffered makes the
     * kernel send RST rather than FIN, and an RST discards whatever the
     * peer has not yet read -- including the response just written. An
     * earlier version of this test replied after a single recv() and
     * failed about half the time for exactly that reason. */
    char     scratch[2048];
    uint32_t used = 0;
    while (used + 1 < sizeof(scratch))
    {
        ssize_t got = recv(fd, scratch + used, sizeof(scratch) - used - 1, 0);
        if (got <= 0)
        {
            break;
        }
        used += (uint32_t)got;
        scratch[used] = '\0';
        if (strstr(scratch, "\r\n\r\n") != NULL)
        {
            break;
        }
    }

    /* Dribble the reply out in small writes so the decoder is forced to
     * refill mid-chunk instead of seeing the whole body at once. That is
     * what exercises the buffer-sharing path. */
    const char* p    = CHUNKED_RESPONSE;
    size_t      left = sizeof(CHUNKED_RESPONSE) - 1;
    while (left > 0)
    {
        size_t  n = (left < 7) ? left : 7;
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0)
        {
            break;
        }
        p    += w;
        left -= (size_t)w;
    }

    /* Orderly close: send FIN, then drain until the peer goes away, so the
     * response is never torn down by an RST. */
    (void)shutdown(fd, SHUT_WR);
    for (;;)
    {
        char    drain[256];
        ssize_t got = recv(fd, drain, sizeof(drain), 0);
        if (got <= 0)
        {
            break;
        }
    }

    (void)close(fd);
    (void)close(listen_fd);
    return ok;
}

static void http_client_decodes_chunked_response(void** state)
{
    (void)state;
    (void)signal(SIGPIPE, SIG_IGN);

    raw_server_ctx_t ctx;
    ctx.port      = PORT_CHUNKED;
    ctx.listening = false;
    assert_int_equal(pthread_mutex_init(&ctx.mutex, NULL), 0);
    assert_int_equal(pthread_cond_init(&ctx.cond, NULL), 0);

    task_t* server_task = task_run(raw_chunked_server, &ctx);
    assert_non_null(server_task);

    assert_true(raw_server_wait_listening(&ctx, 2000));

    test_client_t client;
    client_connect_plain(&client, PORT_CHUNKED);

    send_simple_request(&client, HTTP_METHOD_GET, span_from_str_literal("/"),
                        HTTP_VERSION_1_1, SPAN_EMPTY, true);

    http_response_t response;
    assert_int_equal(http_connection_receive_response(&client.connection,
                                                      client_buffer(&client),
                                                      &response, NULL), ok);

    assert_int_equal(span_compare(response.code, HTTP_CODE_200), 0);
    /* Six chunks reassembled, chunk framing stripped. */
    assert_int_equal(span_compare(response.body,
                                  span_from_str_literal(CHUNKED_EXPECTED)), 0);

    client_disconnect(&client);
    assert_true(task_wait(server_task));
    task_release(server_task);

    (void)pthread_cond_destroy(&ctx.cond);
    (void)pthread_mutex_destroy(&ctx.mutex);
}

/* ------------------------------------------------------------------------- */

int test_http_server()
{
    const struct CMUnitTest tests[] = {
        /* Negative */
        cmocka_unit_test(http_server_init_NULL_server_fails),
        cmocka_unit_test(http_server_init_NULL_config_fails),
        cmocka_unit_test(http_server_init_NULL_storage_fails),
        cmocka_unit_test(http_server_deinit_NULL_fails),
        cmocka_unit_test(http_server_add_route_NULL_server_fails),
        cmocka_unit_test(http_server_add_route_empty_method_fails),
        cmocka_unit_test(http_server_add_route_empty_path_fails),
        cmocka_unit_test(http_server_add_route_NULL_handler_fails),
        cmocka_unit_test(http_server_add_route_overflow_fails),
        cmocka_unit_test(http_server_run_NULL_fails),
        cmocka_unit_test(http_server_stop_NULL_fails),

        /* Positive */
        cmocka_unit_test(http_server_init_and_deinit_succeed),
        cmocka_unit_test(http_server_add_multiple_routes_succeed),
        cmocka_unit_test(http_server_run_lifecycle_succeed),
        cmocka_unit_test(http_server_handles_GET_request_succeed),
        cmocka_unit_test(http_server_handles_POST_with_body_succeed),
        cmocka_unit_test(http_server_path_captures_succeed),
        cmocka_unit_test(http_server_returns_404_for_unknown_path),
        cmocka_unit_test(http_server_returns_405_for_method_mismatch),
        cmocka_unit_test(http_server_keep_alive_multiple_requests_succeed),
        cmocka_unit_test(http_server_handles_parallel_clients_succeed),
        cmocka_unit_test(http_server_plain_http_GET_request_succeed),
        cmocka_unit_test(http_server_streams_large_body_succeed),

        /* Deferred responses */
        cmocka_unit_test(http_response_defer_outside_handler_fails),
        cmocka_unit_test(http_server_respond_rejects_invalid_handles),
        cmocka_unit_test(http_server_deferred_response_from_worker_thread_succeed),
        cmocka_unit_test(http_server_deferred_response_times_out_with_504),
        cmocka_unit_test(http_server_deferred_handle_stale_after_disconnect),
        cmocka_unit_test(http_server_oversize_request_returns_413),

        /* Client-side chunked decoding */
        cmocka_unit_test(http_client_decodes_chunked_response),
    };

    return cmocka_run_group_tests_name("http_server", tests, NULL, NULL);
}
