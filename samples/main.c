#include <stdlib.h>
#include <stdio.h>
#include "http_listener.h"
#include "ssl.h"
#include "common_lib_c.h"



static void a(http_request_t* request)
{
    // do nothing.
}

int main(int argc, char** argv)
{
    // http_server_t http_server;
    // http_server_config_t http_server_config = { 0 };

    // http_server_init(&http_server, &http_server_config);
    // http_server_add_route(&http_server, POST, "/cars/*/*/*", a);
    // http_server_add_route(&http_server, GET, "/index.html", a);
    // http_server_add_route(&http_server, GET, "/", a);
    // http_server_run(&http_server);

    ssl_init();
    ssl_t* ssl = ssl_new();
    uint8_t buffer_raw[1024];
    span_t buffer = span_from_memory(buffer_raw);
    span_t bytes_read;

    while (true)
    {
        ssl_read(ssl, buffer, &bytes_read);

        printf("Bytes read: %.*s\n", span_get_size(bytes_read), span_get_ptr(bytes_read));

        ssl_write(ssl, bytes_read);
    }

    return 0;
}