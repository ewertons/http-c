#ifndef HTTP_LISTENER
#define HTTP_LISTENER

#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>

#include "socket.h"
#include "task.h"
#include "span.h"

#include "http_endpoint.h"
#include "http_request.h"
#include "http_response.h"

#ifndef HTTP_SERVER_MAX_ROUTES
#define HTTP_SERVER_MAX_ROUTES 16
#endif

/* Maximum simultaneous connections served in parallel. Each connection uses
 * one task slot from the global task pool (TASK_POOL_SIZE). */
#ifndef HTTP_SERVER_MAX_CONNECTIONS
#define HTTP_SERVER_MAX_CONNECTIONS 16
#endif

#ifndef HTTP_SERVER_REQUEST_BUFFER_SIZE
#define HTTP_SERVER_REQUEST_BUFFER_SIZE 4096
#endif

#define HTTP_SERVER_DEFAULT_LISTENING_PORT 443

typedef void (*http_request_handler_t)(http_request_t* request, span_t* path_matches, uint16_t number_of_matches, http_response_t* out_response, void* user_context);

typedef enum
{
    http_server_state_initialized = 0x10,
    http_server_state_running     = 0x11,
    http_server_state_stopping    = 0x12,
    http_server_state_stopped     = 0x13
} http_server_state_t;

typedef struct http_route
{
    span_t        method;
    span_t        path;
    span_regex_t  compiled_path;
    http_request_handler_t handler;
    void*         user_context;
} http_route_t;

/* Per-connection worker context. Pre-allocated as part of #http_server_t. */
typedef struct http_server_connection_slot
{
    bool                  in_use;
    struct http_server*   server;
    http_connection_t     connection;
    task_t*               task;
    uint8_t               buffer[HTTP_SERVER_REQUEST_BUFFER_SIZE];
} http_server_connection_slot_t;

typedef struct http_server_config
{
    int port;
    struct
    {
        bool        enable;
        const char* certificate_file;
        const char* private_key_file;
    } tls;
} http_server_config_t;

typedef struct http_server
{
    http_endpoint_config_t local_endpoint_config;
    http_endpoint_t        local_endpoint;

    struct
    {
        http_route_t list[HTTP_SERVER_MAX_ROUTES];
        uint16_t     count;
    } routes;

    http_server_connection_slot_t slots[HTTP_SERVER_MAX_CONNECTIONS];

    http_server_state_t state;
    pthread_mutex_t     state_mutex;
} http_server_t;

static inline http_server_config_t http_server_get_default_config()
{
    http_server_config_t config = { 0 };
    config.tls.enable = true;
    config.port = HTTP_SERVER_DEFAULT_LISTENING_PORT;
    return config;
}

result_t http_server_init(http_server_t* server, http_server_config_t* config);
result_t http_server_deinit(http_server_t* server);

result_t http_server_add_route(http_server_t* server, span_t method, span_t path, http_request_handler_t handler, void* user_context);

/**
 * @brief Runs the HTTP server synchronously (blocking) until #http_server_stop
 *        is called from another thread or the listening socket fails.
 */
result_t http_server_run(http_server_t* server);

/**
 * @brief Runs the HTTP server asynchronously as a task.
 */
task_t* http_server_run_async(http_server_t* server);

/**
 * @brief Requests the server to stop accepting new connections and to drain
 *        in-flight ones. Safe to call from any thread.
 */
result_t http_server_stop(http_server_t* server);

#endif // HTTP_LISTENER
