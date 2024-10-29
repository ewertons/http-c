#include <stdlib.h>
#include "http_listener.h"

void http_server_init(http_server_t* server, http_server_config_t* config)
{
    (void)server;
    (void)config;
}

void http_server_run(http_server_t* server)
{
    (void)server;
}

void http_server_add_route(http_server_t* server, http_method_t method, const char* path, http_request_handler_t handler)
{
    (void)server;
    (void)method;
    (void)path;
    (void)handler;
}
