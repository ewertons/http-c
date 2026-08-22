#ifndef HTTP_LISTENER
#define HTTP_LISTENER

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "socketx.h"
#include "task.h"
#include "span.h"
#include "event_loop.h"

#include "http_endpoint.h"
#include "http_request.h"
#include "http_request_parser.h"
#include "http_response.h"

#define HTTP_SERVER_DEFAULT_LISTENING_PORT 443

/* How long a deferred response may stay parked before the server answers
 * 504 on the handler's behalf. Without a ceiling, an application that
 * loses track of a handle would strand a connection slot forever. */
#define HTTP_SERVER_DEFAULT_DEFERRED_TIMEOUT_MS 30000

/* How long a connection may go silent before the server closes it. Slots
 * are a fixed, never-allocated resource, so an idle keep-alive costs
 * exactly as much as a busy one: with no ceiling, clients that open
 * connections and hold them can take every slot and the listener stops
 * accepting -- alive, logging, and answering nobody. */
#define HTTP_SERVER_DEFAULT_KEEP_ALIVE_TIMEOUT_MS 60000

/* Forward declaration so the state-changed callback typedef below can
 * mention http_server_t* before the struct is defined. */
struct http_server;
typedef struct http_server http_server_t;

/* Forward declaration only -- the exchange holds a slot pointer, and the
 * slot is defined further down. Deliberately not a typedef here: pairing a
 * forward `typedef struct X X;` with a later `typedef struct X {...} X;`
 * is a typedef redefinition, which C99 rejects under -Wpedantic. */
struct http_server_connection_slot;

/**
 * @brief What a handler decided to do with a request.
 *
 * Returning an outcome rather than writing a flag means every exit path
 * has to say what it meant. A handler that hits an error and simply
 * returns cannot accidentally ship whatever half-filled response happened
 * to be in the buffer.
 */
typedef enum http_handler_outcome
{
    /** The response is complete. Send it. */
    http_handler_respond = 0,

    /** Answer later, through the #http_deferral_t taken from the exchange.
     *  The connection is parked; nothing is serialised yet. */
    http_handler_defer,

    /** Drop the connection without answering. */
    http_handler_close,
} http_handler_outcome_t;

/**
 * @brief Ticket for a request whose answer was deferred.
 *
 * Safe to copy and to hold across threads, and self-describing -- it
 * carries the server, so completing one needs nothing else threaded
 * through the application.
 *
 * The generation counter is what makes holding it safe. Connection slots
 * are a fixed, recycled resource, so a completion arriving after its peer
 * disconnected would otherwise be written into whatever connection
 * inherited the slot. Releasing a slot bumps its generation, turning such
 * a late completion into a clean #not_found rather than a response
 * delivered to the wrong client.
 */
typedef struct http_deferral
{
    http_server_t*                      server;
    struct http_server_connection_slot* slot;
    uint32_t                            generation;
} http_deferral_t;

/**
 * @brief Everything a handler is given for one request.
 *
 * Passed instead of loose parameters so the set can grow without breaking
 * handlers again, and so deferral hangs off the request -- which is what
 * it is actually about -- rather than off the response, which is a
 * wire-format type the client half uses too.
 *
 * Treat the fields as private; use the accessors.
 */
typedef struct http_exchange
{
    struct http_server_connection_slot* slot;
    http_request_t*                     request;
    span_t*                             captures;
    uint16_t                            capture_count;
    http_response_t*                    response;
    bool                                defer_requested;
} http_exchange_t;

/**
 * @brief Handles one request.
 *
 * Runs on the server's single event-loop thread and MUST NOT block: while
 * a handler runs, no other connection is served. Anything slow belongs on
 * another thread, with the request deferred until that work finishes.
 */
typedef http_handler_outcome_t (*http_request_handler_t)(http_exchange_t* exchange,
                                                         void*            user_context);

/* Deliberately not const: http_headers_find and friends are not
 * const-correct yet, so returning a const pointer would force a cast at
 * every call site that inspects a header. Const-correcting the request and
 * headers API is worth doing, but as its own change. */
static inline http_request_t* http_exchange_request(http_exchange_t* exchange)
{
    return (exchange != NULL) ? exchange->request : NULL;
}

static inline http_response_t* http_exchange_response(http_exchange_t* exchange)
{
    return (exchange != NULL) ? exchange->response : NULL;
}

static inline uint16_t http_exchange_capture_count(const http_exchange_t* exchange)
{
    return (exchange != NULL) ? exchange->capture_count : (uint16_t)0;
}

/** Bounds-checked; an out-of-range index yields an empty span rather than
 *  reading past the match array. */
static inline span_t http_exchange_capture(const http_exchange_t* exchange, uint16_t index)
{
    if (exchange == NULL || index >= exchange->capture_count)
    {
        return SPAN_EMPTY;
    }
    return exchange->captures[index];
}

/**
 * @brief Take a deferral ticket for this request.
 *
 * Cannot fail: inside a handler there is always a connection to defer.
 * The handler must then return #http_handler_defer -- taking a ticket and
 * returning #http_handler_respond leaves the ticket dangling, which the
 * server logs.
 */
http_deferral_t http_exchange_defer(http_exchange_t* exchange);

typedef enum
{
    http_server_state_initialized = 0x10,
    http_server_state_running     = 0x11,
    http_server_state_stopping    = 0x12,
    http_server_state_stopped     = 0x13
} http_server_state_t;

/**
 * @brief Invoked on every server lifecycle state transition.
 *
 * Fires from inside #http_server_run (i.e. on the server task's thread)
 * for the following transitions:
 *   - http_server_state_running   The listening socket is bound and
 *                                 registered with the event loop. It is
 *                                 safe for clients to begin connecting.
 *   - http_server_state_stopping  #http_server_stop has been called and
 *                                 the event loop is being torn down.
 *   - http_server_state_stopped   The run loop has fully exited and all
 *                                 in-flight connections have been closed.
 *
 * The callback runs on the server's loop thread and must not block or
 * call back into #http_server_run / #http_server_stop. It is intended for
 * lightweight signalling (e.g. setting a #task_completion_source_t).
 */
typedef void (*http_server_on_state_changed_cb)(http_server_t*       server,
                                                http_server_state_t  new_state,
                                                void*                user_context);

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
    http_slot_state_pending,
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

    /* Header storage for the responses the server generates itself (404,
     * 405). A handler brings its own buffer; these have no handler, and
     * without somewhere to write "Content-Length: 0" they go out with no
     * framing at all -- which RFC 7230 3.3.3 says means "body ends when
     * the connection closes", so a keep-alive client waits for a body
     * that is never coming.
     *
     * 32 bytes because that header is 19 of them and this is per slot on
     * a library that never allocates: on the microcontroller storage it
     * is four slots, so the whole fix costs 128 bytes of static RAM. */
    uint8_t               default_headers[32];

    /* Optional streaming body. Active between the serialised head being
     * flushed and the provider reporting end-of-body. See http_response_t
     * (body_provider / body_finalizer). */
    bool                  stream_active;
    bool                  stream_eof;
    /* Set when the handler gave a provider but no Content-Length, and the
     * connection is one that will be reused -- so the body needs framing
     * of its own to say where it ends. */
    bool                  stream_chunked;
    bool                  stream_terminated;
    http_body_provider_t  stream_provider;
    http_body_finalizer_t stream_finalizer;
    void*                 stream_ctx;

    /* Deferred responses (http_response_defer / http_server_respond).
     *
     * `generation` increments every time the slot is released, so a
     * handle held by a worker thread can be recognised as stale rather
     * than delivered to whichever connection reused the slot.
     *
     * `pending_response` lives here rather than in a separate queue so
     * the library keeps its "never allocates" property: a slot can have
     * at most one response in flight, so one inline slot is exactly
     * enough. Fields below are guarded by http_server_t::pending_mutex. */
    uint32_t              generation;
    uint64_t              pending_deadline_ms;
    bool                  pending_ready;
    bool                  pending_cancelled;
    http_response_t       pending_response;

    /* When this connection is closed for going silent. Refreshed every
     * time bytes move in either direction, so it bounds SILENCE rather
     * than duration -- a slow but progressing transfer is never cut off.
     * Not consulted while the slot is parked on a deferral: that wait is
     * the application's, and pending_deadline_ms already bounds it. */
    uint64_t              idle_deadline_ms;
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
    /* Optional. Address to listen on. NULL or empty listens on every
     * interface, which is the default and the historical behaviour.
     *
     * A numeric IPv4 literal only, e.g. "127.0.0.1". It is parsed with
     * inet_pton, so host names ("localhost") and IPv6 literals ("::1") are
     * NOT accepted and make http_server_run fail rather than fall back to
     * listening everywhere -- the reason to set this is to not be reachable,
     * so a silent wildcard would invert the caller's intent. */
    const char* bind_address;
    struct
    {
        bool        enable;
        const char* certificate_file;
        const char* private_key_file;
    } tls;

    /* Optional. Invoked on lifecycle transitions; see
     * #http_server_on_state_changed_cb for the contract. */
    http_server_on_state_changed_cb on_state_changed;
    void*                           on_state_changed_context;

    /* Optional. Ceiling on how long a deferred response may stay parked
     * before the server answers 504 itself. 0 selects
     * #HTTP_SERVER_DEFAULT_DEFERRED_TIMEOUT_MS. */
    uint32_t                        deferred_timeout_ms;

    /* Optional. How long a connection may go silent -- no byte read and
     * none written -- before the server closes it. 0 selects
     * #HTTP_SERVER_DEFAULT_KEEP_ALIVE_TIMEOUT_MS.
     *
     * Covers the TLS handshake, a half-sent request, an idle keep-alive
     * between requests, and a stalled response. It does NOT cover a
     * deferred response: that wait belongs to the application and
     * #deferred_timeout_ms already bounds it.
     *
     * Set UINT32_MAX for a server that must never drop an idle peer. HTTP
     * clients are required to cope with a persistent connection being
     * closed (RFC 7230 6.3.1), so that is rarely what you want. */
    uint32_t                        keep_alive_timeout_ms;
} http_server_config_t;

struct http_server
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
    pthread_cond_t         state_cond;     /* broadcast on every state change */

    /* Cached from config at init time. */
    http_server_on_state_changed_cb state_changed_cb;
    void*                           state_changed_ctx;

    /* Deferred responses. `pending_mutex` guards the per-slot pending_*
     * fields and `generation`, because #http_server_respond may be called
     * from any thread while the loop thread drives the state machine. It
     * is a leaf lock: never take `state_mutex` while holding it. */
    pthread_mutex_t                 pending_mutex;
    uint32_t                        pending_timeout_ms;

    /* Cached from config; see http_server_config_t::keep_alive_timeout_ms. */
    uint32_t                        keep_alive_timeout_ms;
};

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

/**
 * @brief Complete a deferred response. Safe to call from any thread.
 *
 * This is the point of the whole mechanism: a worker finishes its slow
 * work and hands the answer back to the loop thread, which serialises and
 * sends it. The response is copied into the slot and the loop is woken;
 * serialisation itself always happens on the loop thread, so the send
 * buffer is never touched concurrently.
 *
 * Body lifetime: `response` itself is copied, but the memory its spans
 * point at is NOT. It must stay valid until the response has been sent.
 * The caller owns that memory, consistent with the rest of the library.
 *
 * @return #ok on success.
 *         #not_found if the deferral is stale -- the peer disconnected, it
 *         timed out, or the slot was recycled. Expected, not exceptional:
 *         drop the ticket and move on.
 *         #error if it was already completed.
 *         #invalid_argument for a malformed ticket.
 */
result_t http_deferral_complete(http_deferral_t deferral, http_response_t* response);

/**
 * @brief Abandon a deferred response: the server answers 503 and closes.
 *
 * For when the application knows it will never produce an answer (it is
 * shutting down, the resource is gone). Without it the client would wait
 * out the full deferral timeout for a reply that was never coming.
 *
 * @return the same codes as #http_deferral_complete.
 */
result_t http_deferral_cancel(http_deferral_t deferral);

#endif // HTTP_LISTENER
