#ifndef HTTP_LISTENER
#define HTTP_LISTENER

#include <stdlib.h>
#include <stdbool.h>
#include "socket.h"

#include "http_endpoint.h"
#include "http_request.h"
#include "http_response.h"

typedef enum http_method
{
    GET,
    POST,
    PUT,
    DELETE
} http_method_t;

#define MAX_SERVER_ROUTE_COUNT 10

typedef void (*http_request_handler_t)(http_request_t* request, span_t* path_matches, uint16_t number_of_matches, http_response_t* out_response, void* user_context);

typedef struct http_route
{
    http_method_t method;
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
} http_server_t;

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
result_t http_server_add_route(http_server_t* server, http_method_t method, span_t path, http_request_handler_t handler, void* user_context);

/**
 * @brief 
 * 
 * @param server 
 * @return result_t 
 */
result_t http_server_run(http_server_t* server);

#endif // HTTP_LISTENER
