/*
 * http-c sample: client streams a fixed-length request body in chunks.
 *
 * What this sample demonstrates
 * -----------------------------
 *   1. Server on 127.0.0.1:8085 with a POST /upload route.
 *   2. Client opens the connection, serialises only the start-line +
 *      headers (with Content-Length: N), then writes the body to the
 *      connection's underlying stream a chunk at a time. The full body
 *      is never materialised in client-side memory.
 *   3. Server collects the body into its slot's recv buffer and the
 *      route handler reports its size + a small checksum.
 *
 * What this is NOT
 * ----------------
 * This is "fixed-length streaming" -- the client knows the total body
 * length up front and advertises it via Content-Length. The library
 * does not (yet) implement HTTP/1.1 chunked transfer-encoding, and the
 * server delivers the body to the route handler only after the whole
 * thing is received -- there is no streaming receive API today. So:
 *
 *   - Total body size must fit in the server slot's recv buffer
 *     (8 KB by default for the host storage; we send 4 KB here).
 *   - For unknown-length streams the client would need
 *     `Transfer-Encoding: chunked`, which is a planned feature, not a
 *     sample workaround.
 *
 * Cert/key locations are resolved (in order):
 *   - $HTTP_C_SERVER_CERT / $HTTP_C_SERVER_KEY environment variables, or
 *   - ./samples/certs/server.cert.pem / ./samples/certs/server.key.pem
 *     relative to the current working directory.
 *
 * Generate the cert/key with samples/scripts/generate_certs.sh first.
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
#include "http_endpoint.h"
#include "http_connection.h"
#include "http_server.h"
#include "http_server_storage.h"
#include "stream.h"
#include "task.h"

#define DEFAULT_SERVER_CERT_PATH "samples/certs/server.cert.pem"
#define DEFAULT_SERVER_PK_PATH   "samples/certs/server.key.pem"
#define SERVER_PORT              8085
#define SERVER_HOSTNAME          "localhost"

#define BODY_TOTAL_BYTES         4096
#define BODY_CHUNK_BYTES         512   /* 8 chunks of 512 bytes */

static const span_t HOST_HEADER_VALUE = span_from_str_literal("localhost:8085");
static const span_t USER_AGENT_VALUE  = span_from_str_literal("http-c-streaming/1.0");
static const span_t CONTENT_TYPE_OCTET = span_from_str_literal("application/octet-stream");
static const span_t UPLOAD_PATH       = span_from_str_literal("/upload");
static const span_t OK_BODY           = span_from_str_literal("ok\n");
static const span_t OK_BODY_LEN_STR   = span_from_str_literal("3");
static const span_t TEXT_PLAIN        = span_from_str_literal("text/plain");

/* ------------------------------------------------------------------------- *
 * Server-side route handler. Gets the body fully assembled (the library
 * does not surface a streaming-receive API yet), so we just report what
 * we got.
 * ------------------------------------------------------------------------- */

static uint8_t response_headers_buffer[256];

static void upload_handler(http_request_t*  request,
                           span_t*          path_matches,
                           uint16_t         path_match_count,
                           http_response_t* out_response,
                           void*            user_context)
{
    (void)path_matches;
    (void)path_match_count;
    (void)user_context;

    /* Compute a tiny checksum over the body so the test is end-to-end. */
    uint32_t sum = 0;
    const uint8_t* p = span_get_ptr(request->body);
    uint32_t       n = span_get_size(request->body);
    for (uint32_t i = 0; i < n; i++)
    {
        sum = (sum * 31u) + p[i];
    }

    printf("\n----- SERVER received upload -----\n");
    printf("  bytes: %u\n", (unsigned)n);
    printf("  hash : 0x%08x\n", (unsigned)sum);
    fflush(stdout);

    if (http_headers_init(&out_response->headers,
                          span_init(response_headers_buffer,
                                    (uint32_t)sizeof(response_headers_buffer))) != HL_RESULT_OK)
    {
        return;
    }
    (void)http_headers_add(&out_response->headers, HTTP_HEADER_CONTENT_TYPE,   TEXT_PLAIN);
    (void)http_headers_add(&out_response->headers, HTTP_HEADER_CONTENT_LENGTH, OK_BODY_LEN_STR);

    out_response->code          = HTTP_CODE_200;
    out_response->reason_phrase = HTTP_REASON_PHRASE_200;
    out_response->body          = OK_BODY;
}

/* ------------------------------------------------------------------------- *
 * Client task: opens the connection, serialises just the headers, then
 * writes the body in BODY_CHUNK_BYTES-sized writes.
 * ------------------------------------------------------------------------- */

typedef struct client_args
{
    const char* server_cert_path;
    result_t    result;
} client_args_t;

/* Cheap deterministic byte generator so client and server compute the
 * same checksum without sharing any state. */
static uint8_t pattern_byte(uint32_t i)
{
    return (uint8_t)((i * 131u) ^ (i >> 3));
}

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

    /* --- Build request headers. The body is left empty; we will write
     *     it onto the connection's stream after the headers go out. --- */
    char content_length_str[16];
    int  cl_len = snprintf(content_length_str, sizeof(content_length_str),
                           "%d", BODY_TOTAL_BYTES);
    span_t content_length_span = span_init((uint8_t*)content_length_str, (uint32_t)cl_len);

    uint8_t request_header_storage[256];
    http_headers_t request_headers;
    if (http_headers_init(&request_headers,
                          span_init(request_header_storage,
                                    (uint32_t)sizeof(request_header_storage))) != HL_RESULT_OK)
    {
        args->result = error;
        goto cleanup;
    }
    (void)http_headers_add(&request_headers, HTTP_HEADER_HOST,           HOST_HEADER_VALUE);
    (void)http_headers_add(&request_headers, HTTP_HEADER_USER_AGENT,     USER_AGENT_VALUE);
    (void)http_headers_add(&request_headers, HTTP_HEADER_CONTENT_TYPE,   CONTENT_TYPE_OCTET);
    (void)http_headers_add(&request_headers, HTTP_HEADER_CONTENT_LENGTH, content_length_span);
    (void)http_headers_add(&request_headers, HTTP_HEADER_CONNECTION,     span_from_str_literal("close"));

    http_request_t outgoing_request;
    if (http_request_initialize(&outgoing_request,
                                HTTP_METHOD_POST,
                                UPLOAD_PATH,
                                HTTP_VERSION_1_1,
                                request_headers) != ok)
    {
        args->result = error;
        goto cleanup;
    }
    /* Crucial: leave body empty so http_request_serialize_to writes only
     * the start-line, headers, and the blank line that ends them. */
    outgoing_request.body = SPAN_EMPTY;

    printf("\n----- CLIENT sending request (streaming body) -----\n");
    printf("POST /upload HTTP/1.1  Content-Length: %d  (%d chunks of %d bytes)\n",
           BODY_TOTAL_BYTES, BODY_TOTAL_BYTES / BODY_CHUNK_BYTES, BODY_CHUNK_BYTES);
    fflush(stdout);

    if (http_connection_send_request(&connection, &outgoing_request) != ok)
    {
        fprintf(stderr, "client: http_connection_send_request failed\n");
        args->result = error;
        goto cleanup;
    }

    /* --- Stream the body chunk by chunk straight onto the connection's
     *     stream. We never hold the full body in memory. --- */
    uint32_t client_sum = 0;
    uint8_t  chunk[BODY_CHUNK_BYTES];
    for (uint32_t off = 0; off < BODY_TOTAL_BYTES; off += BODY_CHUNK_BYTES)
    {
        for (uint32_t i = 0; i < BODY_CHUNK_BYTES; i++)
        {
            chunk[i] = pattern_byte(off + i);
            client_sum = (client_sum * 31u) + chunk[i];
        }
        span_t chunk_span = span_init(chunk, BODY_CHUNK_BYTES);
        if (stream_write(&connection.stream, chunk_span, NULL) != ok)
        {
            fprintf(stderr, "client: stream_write failed at offset %u\n", (unsigned)off);
            args->result = error;
            goto cleanup;
        }
        printf("  client wrote chunk @%u (%u bytes)\n",
               (unsigned)off, (unsigned)BODY_CHUNK_BYTES);
        fflush(stdout);
    }
    printf("  client done; expected hash 0x%08x\n", (unsigned)client_sum);
    fflush(stdout);

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

    printf("\n----- CLIENT received response -----\n");
    printf("status: ");
    fwrite(span_get_ptr(incoming_response.code), 1,
           span_get_size(incoming_response.code), stdout);
    printf(" ");
    fwrite(span_get_ptr(incoming_response.reason_phrase), 1,
           span_get_size(incoming_response.reason_phrase), stdout);
    printf("\nbody: ");
    fwrite(span_get_ptr(incoming_response.body), 1,
           span_get_size(incoming_response.body), stdout);
    fflush(stdout);

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
 * Server task. Identical structure to the server_client sample.
 * ------------------------------------------------------------------------- */

typedef struct server_args
{
    const char*               cert_path;
    const char*               key_path;
    http_server_t*            server;
    task_completion_source_t* ready;
} server_args_t;

static void on_server_state_changed(http_server_t*       server,
                                    http_server_state_t  new_state,
                                    void*                user_context)
{
    (void)server;
    server_args_t* args = (server_args_t*)user_context;
    if (new_state == http_server_state_running)
    {
        (void)task_completion_source_set_result(args->ready, ok);
    }
}

static result_t run_server(void* state, task_t* self)
{
    (void)self;
    server_args_t* args = (server_args_t*)state;

    http_server_config_t cfg = { 0 };
    cfg.port                       = SERVER_PORT;
    cfg.tls.enable                 = true;
    cfg.tls.certificate_file       = args->cert_path;
    cfg.tls.private_key_file       = args->key_path;
    cfg.on_state_changed           = on_server_state_changed;
    cfg.on_state_changed_context   = args;

    if (http_server_init(args->server, &cfg,
                         http_server_storage_get_for_server_host()) != ok)
    {
        fprintf(stderr, "server: http_server_init failed (cert/key path correct? "
                        "port %d in use?)\n", SERVER_PORT);
        (void)task_completion_source_set_result(args->ready, error);
        return error;
    }

    (void)http_server_add_route(args->server, HTTP_METHOD_POST,
                                span_from_str_literal("^/upload$"),
                                upload_handler, NULL);

    result_t run_result = http_server_run(args->server);
    (void)http_server_deinit(args->server);
    return run_result;
}

/* ------------------------------------------------------------------------- *
 * Entry point. Same boilerplate as the server_client sample.
 * ------------------------------------------------------------------------- */

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    const char* server_cert_path = env_or_default("HTTP_C_SERVER_CERT", DEFAULT_SERVER_CERT_PATH);
    const char* server_key_path  = env_or_default("HTTP_C_SERVER_KEY",  DEFAULT_SERVER_PK_PATH);

    printf("http-c streaming sample\n");
    printf("  server cert: %s\n", server_cert_path);
    printf("  server key : %s\n", server_key_path);
    printf("  listening  : https://%s:%d/upload\n", SERVER_HOSTNAME, SERVER_PORT);
    printf("  body size  : %d bytes (%d chunks of %d)\n",
           BODY_TOTAL_BYTES, BODY_TOTAL_BYTES / BODY_CHUNK_BYTES, BODY_CHUNK_BYTES);

    if (task_platform_init() != ok)
    {
        fprintf(stderr, "task_platform_init failed\n");
        return 1;
    }

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

    if (task_completion_source_wait(&server_ready) != ok)
    {
        (void)task_wait(server_task);
        task_release(server_task);
        (void)task_completion_source_deinit(&server_ready);
        (void)task_platform_deinit();
        return 1;
    }

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

    printf("\n\nDone.\n");
    return 0;
}
