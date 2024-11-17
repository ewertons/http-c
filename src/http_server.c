#include <stdlib.h>
#include "span.h"
#include "http_server.h"
#include "http_methods.h"
#include "http_endpoint.h"
#include "http_connection.h"

result_t http_server_init(http_server_t* server, http_server_config_t* config)
{
    result_t result;

    if (server == NULL || config == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        (void)memset(server, 0, sizeof(http_server_t));

        http_endpoint_config_t* local_endpoint_config = &server->local_endpoint_config;
        local_endpoint_config->port = config->port;
        local_endpoint_config->tls.enable = config->tls.enable;
        local_endpoint_config->tls.certificate_file = config->tls.certificate_file;
        local_endpoint_config->tls.private_key_file = config->tls.private_key_file;

        result = ok;
    }

    return result;
}

result_t http_server_add_route(http_server_t* server, http_method_t method, span_t path, http_request_handler_t handler, void* user_context)
{
    result_t result;

    if (server == NULL || span_is_empty(path) || handler == NULL)
    {
        result = invalid_argument;
    }
    else if (server->routes.count == MAX_SERVER_ROUTE_COUNT)
    {
        result = insufficient_size;
    }
    else
    {
        server->routes.list[server->routes.count].method = method;
        server->routes.list[server->routes.count].path = path;
        server->routes.list[server->routes.count].handler = handler;
        server->routes.list[server->routes.count].user_context = user_context;
        server->routes.count++;

        result = ok;
    }

    return result;
}

static span_t get_method_as_span(http_method_t method)
{
    span_t method_as_span;

    switch(method)
    {
        case GET:
            method_as_span = HTTP_METHOD_GET;
            break;
        case POST:
            method_as_span = HTTP_METHOD_POST;
            break;
        case PUT:
            method_as_span = HTTP_METHOD_PUT;
            break;
        case DELETE:
            method_as_span = HTTP_METHOD_DELETE;
            break;
        default:
            method_as_span = SPAN_EMPTY;
            break;
    }

    return method_as_span;
}

result_t http_server_run(http_server_t* server)
{
    result_t result;

    if (server == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        while (true) // service
        {
            if (is_error(http_endpoint_init(&server->local_endpoint, &(server->local_endpoint_config))))
            {
                // error;
                result = error;
                break;
            }

            while(true) // Keep listening
            {
                http_connection_t connection;
                result_t connection_result = http_endpoint_wait_for_connection(&server->local_endpoint, &connection);

                if (is_success(connection_result))
                {
                    http_request_t request;

                    while (true) // current connection
                    {
                        result_t receive_result = http_connection_receive_request(&connection, &request);

                        if (is_success(receive_result))
                        {
                            for(int i = 0; i < server->routes.count; i++)
                            {
                                span_t route_method = get_method_as_span(server->routes.list[i].method);

                                if (span_compare(route_method, request.method) == 0)
                                {
                                    span_t path_matches[5];
                                    uint16_t number_of_matches;

                                    if (span_regex_is_match(request.path, server->routes.list[i].path, path_matches, sizeofarray(path_matches), &number_of_matches))
                                    {
                                        http_response_t response;
                                        server->routes.list[i].handler(&request, path_matches, number_of_matches, &response, server->routes.list[i].user_context);

                                        if (is_error(http_connection_send_response(&connection, &response)))
                                        {
                                            // error, need to kill connection...
                                            // TODO: exit loop.
                                        }

                                        break;
                                    }
                                }
                            }

                            // Handle if no routes are matched.
                            // TODO: check if request Connection header instructs to close connect, then do it.
                        }
                        else if (receive_result != try_again)
                        {
                            // log error
                            break;
                        }
                    }  // current connection

                    http_connection_close(&connection);
                }
                else if (connection_result != try_again)
                {
                    // error
                    break;
                }
            } // Keep listening

            http_endpoint_deinit(&server->local_endpoint);

        } // service
    }

    return result;
}


// result_t http_server_run(http_server_t* server)
// {
//     result_t result = ok;

//     socket_t client_socket;
//     socket_accept(&server->socket, &client_socket);

//     uint8_t buffer_raw[1024];
//     span_t buffer = span_from_memory(buffer_raw);
//     span_t bytes_read;

//     while (true)
//     {
//         socket_read(&client_socket, buffer, &bytes_read);

//         printf("Bytes read: %.*s\n", span_get_size(bytes_read), span_get_ptr(bytes_read));

//         socket_write(&client_socket, bytes_read);
//     }

//     return result;
// }