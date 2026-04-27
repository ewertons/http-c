#include "http_server_storage.h"

#ifndef HTTP_SERVER_MCU_CONNECTIONS
#define HTTP_SERVER_MCU_CONNECTIONS 4
#endif
#ifndef HTTP_SERVER_MCU_ROUTES
#define HTTP_SERVER_MCU_ROUTES 8
#endif
#ifndef HTTP_SERVER_MCU_REQUEST_BUFFER_SIZE
#define HTTP_SERVER_MCU_REQUEST_BUFFER_SIZE 1024
#endif
#ifndef HTTP_SERVER_MCU_RESPONSE_BUFFER_SIZE
#define HTTP_SERVER_MCU_RESPONSE_BUFFER_SIZE 1024
#endif

static http_server_connection_slot_t s_slots[HTTP_SERVER_MCU_CONNECTIONS];
static http_route_t                  s_routes[HTTP_SERVER_MCU_ROUTES];
static uint8_t                       s_recv_buffers[HTTP_SERVER_MCU_CONNECTIONS]
                                                   [HTTP_SERVER_MCU_REQUEST_BUFFER_SIZE];
static uint8_t                       s_send_buffers[HTTP_SERVER_MCU_CONNECTIONS]
                                                   [HTTP_SERVER_MCU_RESPONSE_BUFFER_SIZE];

static http_server_storage_t s_storage = {
    .slots       = s_slots,
    .slot_count  = HTTP_SERVER_MCU_CONNECTIONS,
    .routes      = s_routes,
    .route_count = HTTP_SERVER_MCU_ROUTES,
};

static int s_initialized = 0;

http_server_storage_t* http_server_storage_get_for_microcontroller(void)
{
    if (!s_initialized)
    {
        for (unsigned i = 0; i < HTTP_SERVER_MCU_CONNECTIONS; i++)
        {
            s_slots[i].recv_buffer_ptr  = s_recv_buffers[i];
            s_slots[i].recv_buffer_size = HTTP_SERVER_MCU_REQUEST_BUFFER_SIZE;
            s_slots[i].send_buffer_ptr  = s_send_buffers[i];
            s_slots[i].send_buffer_size = HTTP_SERVER_MCU_RESPONSE_BUFFER_SIZE;
        }
        s_initialized = 1;
    }
    return &s_storage;
}
