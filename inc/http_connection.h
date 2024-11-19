#ifndef HTTP_CONNECTION_H
#define HTTP_CONNECTION_H

#include <span.h>
#include "niceties.h"

#include "http_endpoint.h"
#include "http_request.h"
#include "http_response.h"



typedef struct http_connection_config
{

} http_connection_config_t;

typedef struct http_connection
{
    http_endpoint_t* endpoint;
    socket_t socket;
} http_connection_t;

result_t http_connection_set_endpoint(http_connection_t* connection, http_endpoint_t* endpoint);

result_t http_connection_get_endpoint(http_connection_t* connection, http_endpoint_t** endpoint);

result_t http_connection_receive_request(http_connection_t* connection, http_request_t* request);

result_t http_connection_send_response(http_connection_t* connection, http_response_t* response);

result_t http_connection_send_request(http_connection_t* connection, http_request_t* request);

result_t http_connection_receive_response(http_connection_t* connection, http_response_t* response);

result_t http_connection_close(http_connection_t* connection);

#endif // HTTP_CONNECTION_H
