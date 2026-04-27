#ifndef HTTP_LISTENER
#define HTTP_LISTENER

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "socket.h"
#include "task.h"
#include "span.h"
#include "event_loop.h"

#include "http_endpoint.h"
#include "http_request.h"
#include "http_request_parser.h"
#include "http_response.h"

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
    bool          in_use;
} http_route_t;

/* Per-slot state machine driven by the event loop. */
typedef enum http_slot_state
{
    http_slot_state_idle = 0,
    http_slot_state_handshaking,
    http_slot_state_receiving,
    http_slot_state_sending,
    http_slot_state_closing,
} http_slot_state_t;

/* Per-connection working memory. The slot, the receive buffer, and the
 * send buffer are owned by the caller (via #http_server_storage_t). The
 * library never allocates. */
typedef struct http_server_connection_slot
{
    bool                  in_use;
    struct http_server*   server;

    /* Connection holding the accepted (non-blocking) socket. */
    http_connection_t     connection;

    /* Caller-owned per-connection buffers. */
    uint8_t*              recv_buffer_ptr;
    uint32_t              recv_buffer_size;
    uint8_t*              send_buffer_ptr;
    uint32_t              send_buffer_size;

    /* State machine. */
    http_slot_state_t     state;
    http_request_parser_t parser;
    uint32_t              recv_used;       /* bytes accumulated in recv buffer */
    uint32_t              send_used;       /* bytes serialised into send buffer */
    uint32_t              send_offset;     /* bytes already pushed onto the wire */
    uint32_t              registered_events; /* event mask currently registered */
    bool                  keep_alive;
    bool                  client_wants_close;
} http_server_connection_slot_t;

/* Caller-supplied storage. The library treats `slots` and `routes` as
 * fixed-size arrays of the corresponding types. Two helper providers
 * declared in `http_server_storage.h` return ready-to-use instances backed
 * by static arrays sized at compile time. Power users may declare their
 * own. */
typedef struct http_server_storage
{
    http_server_connection_slot_t* slots;
    uint32_t                       slot_count;
    http_route_t*                  routes;
    uint32_t                       route_count;
} http_server_storage_t;

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

    http_server_storage_t* storage;
    uint32_t               route_count;     /* used route count */

    /* Single-threaded event loop driving everything: accept, TLS
     * handshake, request reception, response transmission. */
    event_loop_t           loop;
    bool                   listen_registered;

    http_server_state_t    state;
    pthread_mutex_t        state_mutex;
} http_server_t;

static inline http_server_config_t http_server_get_default_config()
{
    http_server_config_t config = { 0 };
    config.tls.enable = true;
    config.port = HTTP_SERVER_DEFAULT_LISTENING_PORT;
    return config;
}

/**
 * @brief Initialize the server with caller-supplied storage.
 *
 * @param storage Required. Use one of the helper providers declared in
 *                http_server_storage.h, or supply your own.
 */
result_t http_server_init(http_server_t* server, http_server_config_t* config, http_server_storage_t* storage);
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
