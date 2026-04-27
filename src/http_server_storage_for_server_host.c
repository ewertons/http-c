#include "http_server_storage.h"

#ifndef HTTP_SERVER_HOST_CONNECTIONS
#define HTTP_SERVER_HOST_CONNECTIONS 256
#endif
#ifndef HTTP_SERVER_HOST_ROUTES
#define HTTP_SERVER_HOST_ROUTES 32
#endif
#ifndef HTTP_SERVER_HOST_REQUEST_BUFFER_SIZE
#define HTTP_SERVER_HOST_REQUEST_BUFFER_SIZE 8192
#endif

static http_server_connection_slot_t s_slots[HTTP_SERVER_HOST_CONNECTIONS];
static http_route_t                  s_routes[HTTP_SERVER_HOST_ROUTES];
static uint8_t                       s_buffers[HTTP_SERVER_HOST_CONNECTIONS]
                                              [HTTP_SERVER_HOST_REQUEST_BUFFER_SIZE];

static http_server_storage_t s_storage = {
    .slots       = s_slots,
    .slot_count  = HTTP_SERVER_HOST_CONNECTIONS,
    .routes      = s_routes,
    .route_count = HTTP_SERVER_HOST_ROUTES,
};

static int s_initialized = 0;

http_server_storage_t* http_server_storage_get_for_server_host(void)
{
    if (!s_initialized)
    {
        for (unsigned i = 0; i < HTTP_SERVER_HOST_CONNECTIONS; i++)
        {
            s_slots[i].buffer_ptr  = s_buffers[i];
            s_slots[i].buffer_size = HTTP_SERVER_HOST_REQUEST_BUFFER_SIZE;
        }
        s_initialized = 1;
    }
    return &s_storage;
}
