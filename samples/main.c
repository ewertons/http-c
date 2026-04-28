/*
 * http-c sample: in-process HTTPS server + client with wire-level tracing.
 *
 * What this sample does:
 *   1. Starts an HTTPS server on 127.0.0.1:8084 with three routes.
 *   2. Connects an HTTPS client to it (in the same process, on a worker task).
 *   3. Issues a GET / request, prints the wire-level HTTP traffic seen on
 *      both sides (client and server), and shuts everything down cleanly.
 *
 * Cert/key locations are resolved (in order):
 *   - $HTTP_C_SERVER_CERT / $HTTP_C_SERVER_KEY environment variables, or
 *   - ./samples/certs/server.cert.pem and ./samples/certs/server.key.pem
 *     relative to the current working directory.
 *
 * Generate the cert/key with samples/scripts/generate_certs.sh before
 * running. See samples/README.md for full instructions.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "common_lib_c.h"

#include <http_request.h>
#include <http_response.h>
#include "http_methods.h"
#include "http_codes.h"
#include "http_versions.h"
#include "http_headers.h"
#include "http_connection.h"
#include "http_endpoint.h"
#include "http_server.h"
#include "http_server_storage.h"
#include "task.h"

#define DEFAULT_SERVER_CERT_PATH "samples/certs/server.cert.pem"
#define DEFAULT_SERVER_PK_PATH   "samples/certs/server.key.pem"
#define SERVER_PORT              8084
#define SERVER_HOSTNAME          "localhost"

static const span_t HELLO_BODY            = span_from_str_literal("hello\n");
static const span_t HELLO_BODY_LEN_STR    = span_from_str_literal("6");
static const span_t CONTENT_TYPE_TEXT     = span_from_str_literal("text/plain");
static const span_t HOST_HEADER_VALUE     = span_from_str_literal("localhost:8084");
static const span_t USER_AGENT_VALUE      = span_from_str_literal("http-c-sample/1.0");

/* ------------------------------------------------------------------------- *
 * Pretty-printing helpers. We render the parsed request/response back into
 * an HTTP/1.1 wire-format approximation so the user can see what crossed
 * the TLS tunnel.
 * ------------------------------------------------------------------------- */

static void print_span(span_t s)
{
    fwrite(span_get_ptr(s), 1, span_get_size(s), stdout);
}

static void print_banner(const char* who, const char* direction)
{
    printf("\n----- %s %s -----\n", who, direction);
}

static void print_headers(http_headers_t* headers)
{
    span_t name, value;
    while (http_headers_get_next(headers, &name, &value) == HL_RESULT_OK)
    {
        print_span(name);
        printf(": ");
        print_span(value);
        printf("\r\n");
    }
}

static void print_http_request(http_request_t* request)
{
    print_span(request->method);
    printf(" ");
    print_span(request->path);
    printf(" ");
    print_span(request->http_version);
    printf("\r\n");
    print_headers(&request->headers);
    printf("\r\n");
    if (!span_is_empty(request->body))
    {
        print_span(request->body);
        printf("\n");
    }
    fflush(stdout);
}

static void print_http_response(http_response_t* response)
{
    print_span(response->http_version);
    printf(" ");
    print_span(response->code);
    printf(" ");
    print_span(response->reason_phrase);
    printf("\r\n");
    print_headers(&response->headers);
    printf("\r\n");
    if (!span_is_empty(response->body))
    {
        print_span(response->body);
        printf("\n");
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------------- *
 * Server-side route handler.
 * ------------------------------------------------------------------------- */

static uint8_t response_headers_buffer[256];

static void hello_handler(http_request_t*  request,
                          span_t*          path_matches,
                          uint16_t         path_match_count,
                          http_response_t* out_response,
                          void*            user_context)
{
    (void)path_matches;
    (void)path_match_count;
    (void)user_context;

    print_banner("SERVER", "received request");
    print_http_request(request);

    /* The framework pre-fills out_response as HTTP/1.1 200 OK with empty
     * headers. Attach a header buffer and add Content-Length and
     * Content-Type so the client can read the body. */
    if (http_headers_init(&out_response->headers,
                          span_init(response_headers_buffer,
                                    (uint32_t)sizeof(response_headers_buffer))) != HL_RESULT_OK)
    {
        return;
    }

    (void)http_headers_add(&out_response->headers,
                           HTTP_HEADER_CONTENT_TYPE, CONTENT_TYPE_TEXT);
    (void)http_headers_add(&out_response->headers,
                           HTTP_HEADER_CONTENT_LENGTH, HELLO_BODY_LEN_STR);

    out_response->code          = HTTP_CODE_200;
    out_response->reason_phrase = HTTP_REASON_PHRASE_200;
    out_response->body          = HELLO_BODY;
}

/* ------------------------------------------------------------------------- *
 * Client task: connects, sends a GET /, receives and prints the response.
 * ------------------------------------------------------------------------- */

typedef struct client_args
{
    const char* server_cert_path; /* used as a trust anchor for the self-signed server cert */
    result_t    result;
} client_args_t;

static result_t run_client(void* state, task_t* self)
{
    (void)self;
    client_args_t* args = (client_args_t*)state;

    http_endpoint_t endpoint;
    http_endpoint_config_t endpoint_config = http_endpoint_get_default_secure_client_config();
    endpoint_config.remote.hostname = span_from_str_literal(SERVER_HOSTNAME);
    endpoint_config.remote.port     = SERVER_PORT;
    endpoint_config.tls.trusted_certificate_file = args->server_cert_path;

    if (http_endpoint_init(&endpoint, &endpoint_config) != ok)
    {
        fprintf(stderr, "client: http_endpoint_init failed\n");
        args->result = error;
        return error;
    }

    http_connection_t connection;
    if (http_endpoint_connect(&endpoint, &connection) != ok)
    {
        fprintf(stderr, "client: http_endpoint_connect failed\n");
        (void)http_endpoint_deinit(&endpoint);
        args->result = error;
        return error;
    }

    /* --- Build and send the request. --- */
    uint8_t request_header_storage[256];
    http_headers_t request_headers;
    if (http_headers_init(&request_headers,
                          span_init(request_header_storage,
                                    (uint32_t)sizeof(request_header_storage))) != HL_RESULT_OK)
    {
        args->result = error;
        goto cleanup;
    }
    (void)http_headers_add(&request_headers, HTTP_HEADER_HOST,       HOST_HEADER_VALUE);
    (void)http_headers_add(&request_headers, HTTP_HEADER_USER_AGENT, USER_AGENT_VALUE);
    (void)http_headers_add(&request_headers, HTTP_HEADER_ACCEPT,     span_from_str_literal("*/*"));
    (void)http_headers_add(&request_headers, HTTP_HEADER_CONNECTION, span_from_str_literal("close"));

    http_request_t outgoing_request;
    if (http_request_initialize(&outgoing_request,
                                HTTP_METHOD_GET,
                                span_from_str_literal("/"),
                                HTTP_VERSION_1_1,
                                request_headers) != ok)
    {
        args->result = error;
        goto cleanup;
    }

    print_banner("CLIENT", "sending request");
    print_http_request(&outgoing_request);

    if (http_connection_send_request(&connection, &outgoing_request) != ok)
    {
        fprintf(stderr, "client: http_connection_send_request failed\n");
        args->result = error;
        goto cleanup;
    }

    /* --- Receive and print the response. --- */
    static uint8_t recv_buffer[2048];
    http_response_t incoming_response;
    if (http_connection_receive_response(&connection,
                                         span_init(recv_buffer, (uint32_t)sizeof(recv_buffer)),
                                         &incoming_response,
                                         NULL) != ok)
    {
        fprintf(stderr, "client: http_connection_receive_response failed\n");
        args->result = error;
        goto cleanup;
    }

    print_banner("CLIENT", "received response");
    print_http_response(&incoming_response);

    args->result = ok;

cleanup:
    (void)http_connection_close(&connection);
    (void)http_endpoint_deinit(&endpoint);
    return args->result;
}

static const char* env_or_default(const char* name, const char* fallback)
{
    const char* v = getenv(name);
    return (v != NULL && v[0] != '\0') ? v : fallback;
}

/* ------------------------------------------------------------------------- *
 * Server task: configures routes, runs the event loop until stopped.
 * ------------------------------------------------------------------------- */

typedef struct server_args
{
    const char*               cert_path;
    const char*               key_path;
    http_server_t*            server;
    task_completion_source_t* ready;   /* signalled with the init result_t */
} server_args_t;

static result_t run_server(void* state, task_t* self)
{
    (void)self;
    server_args_t* args = (server_args_t*)state;

    http_server_config_t cfg = { 0 };
    cfg.port                 = SERVER_PORT;
    cfg.tls.enable           = true;
    cfg.tls.certificate_file = args->cert_path;
    cfg.tls.private_key_file = args->key_path;

    if (http_server_init(args->server, &cfg,
                         http_server_storage_get_for_server_host()) != ok)
    {
        fprintf(stderr, "server: http_server_init failed (cert/key path correct? "
                        "port %d in use?)\n", SERVER_PORT);
        (void)task_completion_source_set_result(args->ready, error);
        return error;
    }

    (void)http_server_add_route(args->server, HTTP_METHOD_POST,
                                span_from_str_literal("^/cars/.*/.*/.*$"),
                                hello_handler, NULL);
    (void)http_server_add_route(args->server, HTTP_METHOD_GET,
                                span_from_str_literal("^/index\\.html$"),
                                hello_handler, NULL);
    (void)http_server_add_route(args->server, HTTP_METHOD_GET,
                                span_from_str_literal("^/$"),
                                hello_handler, NULL);

    /* Tell the main thread it is safe to start issuing requests. The server
     * task itself keeps running until http_server_stop() is called. */
    (void)task_completion_source_set_result(args->ready, ok);

    /* Blocks here until http_server_stop() is called from the main thread. */
    result_t run_result = http_server_run(args->server);

    (void)http_server_deinit(args->server);
    return run_result;
}

/* ------------------------------------------------------------------------- *
 * Entry point.
 * ------------------------------------------------------------------------- */

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    const char* server_cert_path = env_or_default("HTTP_C_SERVER_CERT", DEFAULT_SERVER_CERT_PATH);
    const char* server_key_path  = env_or_default("HTTP_C_SERVER_KEY",  DEFAULT_SERVER_PK_PATH);

    printf("http-c sample\n");
    printf("  server cert: %s\n", server_cert_path);
    printf("  server key : %s\n", server_key_path);
    printf("  listening  : https://%s:%d/\n", SERVER_HOSTNAME, SERVER_PORT);

    if (task_platform_init() != ok)
    {
        fprintf(stderr, "task_platform_init failed\n");
        return 1;
    }

    /* --- Spawn the server task and wait for its readiness signal. --- */
    http_server_t            http_server;
    task_completion_source_t server_ready;
    if (task_completion_source_init(&server_ready) != ok)
    {
        fprintf(stderr, "task_completion_source_init failed\n");
        (void)task_platform_deinit();
        return 1;
    }

    server_args_t server_args = {
        .cert_path = server_cert_path,
        .key_path  = server_key_path,
        .server    = &http_server,
        .ready     = &server_ready,
    };

    task_t* server_task = task_run(run_server, &server_args);
    if (server_task == NULL)
    {
        fprintf(stderr, "task_run(run_server) failed\n");
        (void)task_completion_source_deinit(&server_ready);
        (void)task_platform_deinit();
        return 1;
    }

    /* Block until run_server publishes its init outcome. */
    if (task_completion_source_wait(&server_ready) != ok)
    {
        (void)task_wait(server_task);
        task_release(server_task);
        (void)task_completion_source_deinit(&server_ready);
        (void)task_platform_deinit();
        return 1;
    }

    /* Give the run loop a moment to enter the accepting state. */
    task_sleep_ms(50);

    /* --- Run the client on a worker task. --- */
    client_args_t client_args = {
        .server_cert_path = server_cert_path,
        .result           = error,
    };

    task_t* client_task = task_run(run_client, &client_args);
    if (client_task == NULL)
    {
        fprintf(stderr, "task_run(run_client) failed\n");
        (void)http_server_stop(&http_server);
        (void)task_wait(server_task);
        task_release(server_task);
        (void)task_completion_source_deinit(&server_ready);
        (void)task_platform_deinit();
        return 1;
    }

    (void)task_wait(client_task);
    result_t client_result = task_get_result(client_task);
    task_release(client_task);

    /* --- Shut the server down cleanly. --- */
    (void)http_server_stop(&http_server);
    (void)task_wait(server_task);
    task_release(server_task);
    (void)task_completion_source_deinit(&server_ready);
    (void)task_platform_deinit();

    if (client_result != ok)
    {
        fprintf(stderr, "\nClient run reported a failure.\n");
        return 1;
    }

    printf("\nDone.\n");
    return 0;
}
