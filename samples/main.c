#include <stdlib.h>
#include <stdio.h>

#include "common_lib_c.h"

#include <http_request.h>
#include <http_response.h>
#include "http_methods.h"
#include "http_server.h"

#define SERVER_CERT_PATH "TBD"
#define SERVER_PK_PATH "TBD"

static void a(http_request_t* request, span_t* span_matches, uint16_t span_matches_count, http_response_t* out_response, void* user_context)
{
    (void)request;
    (void)span_matches;
    (void)span_matches_count;
    (void)out_response;
    (void)user_context;
    // do nothing.
}

int main(int argc, char** argv)
{
    http_server_config_t http_server_config = { 0 };
    http_server_config.port = 8084;
    http_server_config.tls.enable = true;
    http_server_config.tls.certificate_file = SERVER_CERT_PATH;
    http_server_config.tls.private_key_file = SERVER_PK_PATH;

    http_server_t http_server; 

    http_server_init(&http_server, &http_server_config);
    http_server_add_route(&http_server, HTTP_METHOD_POST, span_from_str_literal("/cars/.*/.*/.*"), a, NULL);
    http_server_add_route(&http_server, HTTP_METHOD_GET, span_from_str_literal("/index.html"), a, NULL);
    http_server_add_route(&http_server, HTTP_METHOD_GET, span_from_str_literal("/"), a, NULL);
    http_server_run(&http_server);

    return 0;
}
