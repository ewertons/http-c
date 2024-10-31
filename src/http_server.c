#include <stdlib.h>
#include "span.h"
#include "http_server.h"

int http_server_init(http_server_t* server, http_server_config_t* config)
{
    server->socket_config = socket_get_default_secure_listener_config();
    server->socket_config.local.port = config->port;
    server->socket_config.tls.enable = config->tls.enable;
    server->socket_config.tls.certificate_file = config->tls.certificate_file;
    server->socket_config.tls.private_key_file = config->tls.private_key_file;

    if (socket_init(&server->socket, &server->socket_config) != 0)
    {
        return ERROR;
    }
    else
    {
        return OK;
    }
}

void http_server_run(http_server_t* server)
{
    socket_t client_socket;
    socket_accept(&server->socket, &client_socket);

    uint8_t buffer_raw[1024];
    span_t buffer = span_from_memory(buffer_raw);
    span_t bytes_read;

    while (true)
    {
        socket_read(&client_socket, buffer, &bytes_read);

        printf("Bytes read: %.*s\n", span_get_size(bytes_read), span_get_ptr(bytes_read));

        socket_write(&client_socket, bytes_read);
    }
}

void http_server_add_route(http_server_t* server, http_method_t method, const char* path, http_request_handler_t handler)
{
    (void)server;
    (void)method;
    (void)path;
    (void)handler;
}
