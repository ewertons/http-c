/*
 * Benchmark server using http-c. Single-threaded epoll event loop with TLS
 * via OpenSSL (the same configuration as production users would deploy).
 *
 * Responds to GET / with a fixed "hello\n" body and an explicit
 * Content-Length so that load generators can use HTTP/1.1 keep-alive.
 */

#include <stdio.h>
#include <stdlib.h>

#include "common_lib_c.h"

#include "http_methods.h"
#include "http_codes.h"
#include "http_versions.h"
#include "http_headers.h"
#include "http_server.h"
#include "http_server_storage.h"
#include "task.h"

#ifndef BENCH_CERT_PATH
#define BENCH_CERT_PATH "/tmp/http-c-certs/server/server.cert.pem"
#endif
#ifndef BENCH_KEY_PATH
#define BENCH_KEY_PATH  "/tmp/http-c-certs/server/server.key.pem"
#endif
#ifndef BENCH_PORT
#define BENCH_PORT 8443
#endif

static const span_t BODY = span_from_str_literal("hello\n");

static void hello_handler(http_request_t* request,
                          span_t* matches, uint16_t match_count,
                          http_response_t* out, void* ctx)
{
    (void)request; (void)matches; (void)match_count; (void)ctx;

    static const span_t CONTENT_LENGTH_VAL = span_from_str_literal("6");
    static const span_t CONTENT_TYPE_VAL   = span_from_str_literal("text/plain");

    /* Small per-call header buffer; storage is on the call stack which is
     * fine because the response is serialised before this function returns. */
    static __thread uint8_t hdr_storage[128];
    http_headers_t headers;
    http_headers_init(&headers, span_init(hdr_storage, sizeof(hdr_storage)));
    http_headers_add(&headers, HTTP_HEADER_CONTENT_LENGTH, CONTENT_LENGTH_VAL);
    http_headers_add(&headers, HTTP_HEADER_CONTENT_TYPE,   CONTENT_TYPE_VAL);

    out->code          = HTTP_CODE_200;
    out->reason_phrase = HTTP_REASON_PHRASE_200;
    out->headers       = headers;
    out->body          = BODY;
}

static http_server_t g_server;

int main(void)
{
    if (task_platform_init() != ok)
    {
        fprintf(stderr, "task_platform_init failed\n");
        return 1;
    }

    http_server_config_t cfg = { 0 };
    int port = BENCH_PORT;
    const char* env_port = getenv("BENCH_PORT");
    if (env_port != NULL) port = atoi(env_port);
    cfg.port = port;
    cfg.tls.enable = true;
    cfg.tls.certificate_file = BENCH_CERT_PATH;
    cfg.tls.private_key_file = BENCH_KEY_PATH;

    if (http_server_init(&g_server, &cfg, http_server_storage_get_for_server_host()) != ok)
    {
        fprintf(stderr, "http_server_init failed\n");
        return 1;
    }
    http_server_add_route(&g_server, HTTP_METHOD_GET,
                          span_from_str_literal("^/$"),
                          hello_handler, NULL);

    fprintf(stderr, "http-c bench listening on port %d (TLS)\n", port);
    http_server_run(&g_server);
    http_server_deinit(&g_server);
    (void)task_platform_deinit();
    return 0;
}
