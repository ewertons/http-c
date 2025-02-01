#ifndef HTTP_LISTENER
#define HTTP_LISTENER

#include <stdlib.h>
#include <stdbool.h>
#include "socket.h"

#include "http_endpoint.h"
#include "http_request.h"
#include "http_response.h"

#define MAX_SERVER_ROUTE_COUNT 10
#define HTTP_SERVER_DEFAULT_LISTENING_PORT 443

typedef void (*http_request_handler_t)(http_request_t* request, span_t* path_matches, uint16_t number_of_matches, http_response_t* out_response, void* user_context);

typedef enum
{
    http_server_state_initialized = 0x10,
    http_server_state_running = 0x11,
    http_server_state_stopped = 0x12
} http_server_state_t;

typedef struct http_route
{
    span_t method;
    span_t path;
    http_request_handler_t handler;
    void* user_context;
} http_route_t;

typedef struct http_server_config
{
    int port;
    struct
    {
        bool enable;
        const char* certificate_file;
        const char* private_key_file;
    } tls;
} http_server_config_t;

typedef struct http_server
{
    http_endpoint_config_t local_endpoint_config;
    http_endpoint_t local_endpoint;

    struct
    {
        http_route_t list[MAX_SERVER_ROUTE_COUNT];
        uint16_t count;
    } routes;

    http_server_state_t state;
} http_server_t;

static inline http_server_config_t http_server_get_default_config()
{
    http_server_config_t config = { 0 };
    config.tls.enable = true;
    config.port = HTTP_SERVER_DEFAULT_LISTENING_PORT;
    return config;
}

/**
 * @brief 
 * 
 * @param server 
 * @param config 
 * @return result_t 
 */
result_t http_server_init(http_server_t* server, http_server_config_t* config);

/**
 * @brief 
 * 
 * @param server 
 * @param method 
 * @param path 
 * @param handler 
 * @param user_context 
 * @return result_t 
 */
result_t http_server_add_route(http_server_t* server, span_t method, span_t path, http_request_handler_t handler, void* user_context);

/**
 * @brief Runs the HTTP server synchronously (blocking).
 * 
 * @param server The #http_server_t instance to be run.
 * @return result_t indicating the result of the call.
 */
result_t http_server_run(http_server_t* server);

/**
 * @brief Runs the HTTP server asynchronously as a task.
 * 
 * @param server The #http_server_t instance to be run.
 * @return task_t* as the handle to the async task.
 */
task_t* http_server_run_async(http_server_t* server);

#endif // HTTP_LISTENER
