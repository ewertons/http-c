#ifndef HTTP_LISTENER
#define HTTP_LISTENER


typedef enum http_method
{
    GET,
    POST,
    PUT,
    DELETE
} http_method_t;


typedef struct http_server_config
{
    int port;
} http_server_config_t;

typedef struct http_request
{
    http_method_t method;
    char* path;
} http_request_t;

typedef void (*http_request_handler_t)(http_request_t* request);

typedef struct http_route
{
    http_method_t method;
    char* path;
    http_request_handler_t handler;
} http_route_t;

typedef struct http_server
{
    int a;
} http_server_t;


void http_server_init(http_server_t* server, http_server_config_t* config);

void http_server_run(http_server_t* server);

void http_server_add_route(http_server_t* server, http_method_t method, const char* path, http_request_handler_t handler);


#endif // HTTP_LISTENER
