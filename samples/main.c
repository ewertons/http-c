#include <stdlib.h>
#include <stdio.h>

#include "common_lib_c.h"

#include <http_request.h>
#include <http_response.h>
#include "http_methods.h"
#include "http_codes.h"
#include "http_versions.h"
#include "http_headers.h"
#include "http_server.h"
#include "http_server_storage.h"
#include "task.h"

#define SERVER_CERT_PATH "/tmp/http-c-certs/server/server.cert.pem"
#define SERVER_PK_PATH "/tmp/http-c-certs/server/server.key.pem"

static const span_t HELLO_BODY = span_from_str_literal("hello\n");

static void a(http_request_t* request, span_t* span_matches, uint16_t span_matches_count, http_response_t* out_response, void* user_context)
{
    (void)request;
    (void)span_matches;
    (void)span_matches_count;
    (void)user_context;

    /* The framework pre-fills `out_response` with HTTP/1.1 200 OK. We can
     * override status, headers, and body. */
    out_response->code          = HTTP_CODE_200;
    out_response->reason_phrase = HTTP_REASON_PHRASE_200;
    out_response->body          = HELLO_BODY;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    if (task_platform_init() != ok)
    {
        fprintf(stderr, "task_platform_init failed\n");
        return 1;
    }

    http_server_config_t http_server_config = { 0 };
    http_server_config.port = 8084;
    http_server_config.tls.enable = true;
    http_server_config.tls.certificate_file = SERVER_CERT_PATH;
    http_server_config.tls.private_key_file = SERVER_PK_PATH;

    http_server_t http_server;

    http_server_init(&http_server, &http_server_config, http_server_storage_get_for_server_host());
    http_server_add_route(&http_server, HTTP_METHOD_POST, span_from_str_literal("^/cars/.*/.*/.*$"), a, NULL);
    http_server_add_route(&http_server, HTTP_METHOD_GET,  span_from_str_literal("^/index\\.html$"),   a, NULL);
    http_server_add_route(&http_server, HTTP_METHOD_GET,  span_from_str_literal("^/$"),               a, NULL);
    http_server_run(&http_server);
    http_server_deinit(&http_server);

    (void)task_platform_deinit();
    return 0;
}
