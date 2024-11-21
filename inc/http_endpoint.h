#ifndef HTTP_ENDPOINT_H
#define HTTP_ENDPOINT_H

#include <span.h>

#include "niceties.h"
#include "socket.h"
#include "task.h"

typedef enum http_endpoint_role
{
    http_endpoint_client,
    http_endpoint_server
} http_endpoint_role_t;

typedef struct http_endpoint_config
{
    http_endpoint_role_t role;
    span_t hostname;
    int port;
    struct
    {
        bool enable;
        const char* certificate_file;
        const char* private_key_file;
    } tls;
} http_endpoint_config_t;

typedef struct http_endpoint
{
    http_endpoint_role_t role;
    socket_config_t socket_config;
    socket_t socket;
} http_endpoint_t;

#include "http_connection.h"

result_t http_endpoint_init(http_endpoint_t* endpoint, http_endpoint_config_t* config);

result_t http_endpoint_wait_for_connection(http_endpoint_t* endpoint, http_connection_t* connection);
task_t* http_endpoint_wait_for_connection_async(http_endpoint_t* endpoint, http_connection_t* connection);

result_t http_endpoint_connect(http_endpoint_t* endpoint, http_connection_t* connection);

result_t http_endpoint_deinit(http_endpoint_t* endpoint);

#endif // HTTP_ENDPOINT_H
