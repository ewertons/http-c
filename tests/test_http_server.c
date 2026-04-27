#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <inttypes.h>
#include <string.h>

#include <pthread.h>

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

/* Synchronization between handler invocation and the test driver. */
typedef struct handler_capture
{
    pthread_mutex_t mutex;
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
    };

    return cmocka_run_group_tests_name("http_server", tests, NULL, NULL);
}
