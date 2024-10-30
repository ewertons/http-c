#include <stdlib.h>
#include <stdio.h>
#include "http_listener.h"
#include "socket.h"
#include "common_lib_c.h"



static void a(http_request_t* request)
{
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
    http_server_add_route(&http_server, POST, "/cars/*/*/*", a);
    http_server_add_route(&http_server, GET, "/index.html", a);
    http_server_add_route(&http_server, GET, "/", a);
    http_server_run(&http_server);

    return 0;
}