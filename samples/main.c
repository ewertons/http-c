#include <stdlib.h>
#include <stdio.h>
#include "http_server.h"
#include "socket.h"
#include "common_lib_c.h"

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
    http_server_config.tls.certificate_file = "/home/ewertons/srv-cert.pem";
    http_server_config.tls.private_key_file = "/home/ewertons/srv-priv.pem";

    http_server_t http_server; 

    http_server_init(&http_server, &http_server_config);
    http_server_add_route(&http_server, POST, span_from_str_literal("/cars/.*/.*/.*"), a, NULL);
    http_server_add_route(&http_server, GET, span_from_str_literal("/index.html"), a, NULL);
    http_server_add_route(&http_server, GET, span_from_str_literal("/"), a, NULL);
    http_server_run(&http_server);

    return 0;
}
