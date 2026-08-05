#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "common_lib_c.h"

#include "http_server.h"
#include "http_methods.h"
#include "http_endpoint.h"
#include "http_connection.h"
#include "http_codes.h"
#include "http_versions.h"
#include "http_headers.h"
#include "http_request_parser.h"
#include "common.h"
#include "event_loop.h"
#include "logging.h"

/* ------------------------------------------------------------------------- *
 * Forward declarations.
 * ------------------------------------------------------------------------- */
static void on_listen_readable(int fd, uint32_t events, void* user);
static void on_connection_io  (int fd, uint32_t events, void* user);
static void slot_close        (http_server_connection_slot_t* slot);
static void slot_advance      (http_server_connection_slot_t* slot);
static void slot_arm          (http_server_connection_slot_t* slot, uint32_t events);
static void slot_drive_handshake(http_server_connection_slot_t* slot);
static void stream_finalize   (http_server_connection_slot_t* slot);
static void run_handler_and_serialize(http_server_connection_slot_t* slot,
                                      http_request_t*               request);
static void serialize_and_send(http_server_connection_slot_t* slot,
                               http_response_t*              response);
static void respond_with_status(http_server_connection_slot_t* slot,
                                span_t code, span_t reason);

/* ------------------------------------------------------------------------- *
 * Internal helpers.
 * ------------------------------------------------------------------------- */
static const span_t HDR_CONNECTION = span_from_str_literal("Connection");
static const span_t HDR_VAL_CLOSE  = span_from_str_literal("close");
static const span_t HDR_VAL_ZERO   = span_from_str_literal("0");

/* Monotonic milliseconds. CLOCK_MONOTONIC rather than CLOCK_REALTIME so a
 * wall-clock adjustment cannot make a deferral deadline fire early or
 * never. */
static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static http_server_state_t server_get_state(http_server_t* server)
{
    http_server_state_t s;
    (void)pthread_mutex_lock(&server->state_mutex);
    s = server->state;
    (void)pthread_mutex_unlock(&server->state_mutex);
    return s;
}

static void server_set_state(http_server_t* server, http_server_state_t s)
{
    (void)pthread_mutex_lock(&server->state_mutex);
    server->state = s;
    /* Wake anyone in http_server_stop or another waiter. The broadcast
     * happens under the mutex so the predicate check on the wake side
     * always sees this exact transition. */
    (void)pthread_cond_broadcast(&server->state_cond);
    (void)pthread_mutex_unlock(&server->state_mutex);

    /* The callback fields are configured once at init time and never
     * mutated afterwards, so no locking is required. The callback runs
     * outside the state mutex so it can safely take other locks. */
    if (server->state_changed_cb != NULL)
    {
        server->state_changed_cb(server, s, server->state_changed_ctx);
    }
}

/* Returns NULL if no slot is available. */
static http_server_connection_slot_t* acquire_slot(http_server_t* server)
{
    for (uint32_t i = 0; i < server->storage->slot_count; i++)
    {
        if (!server->storage->slots[i].in_use)
        {
            http_server_connection_slot_t* slot = &server->storage->slots[i];
            slot->in_use             = true;
            slot->server             = server;
            slot->state              = http_slot_state_idle;
            slot->recv_used          = 0;
            slot->send_used          = 0;
            slot->send_offset        = 0;
            slot->registered_events  = 0;
            slot->keep_alive         = false;
            slot->client_wants_close = false;
            slot->stream_active      = false;
            slot->stream_eof         = false;
            slot->stream_provider    = NULL;
            slot->stream_finalizer   = NULL;
            slot->stream_ctx         = NULL;
            /* NB: `generation` is deliberately not reset here -- it must
             * keep advancing across reuses for handle validation to mean
             * anything. */
            slot->pending_ready       = false;
            slot->pending_cancelled   = false;
            slot->pending_deadline_ms = 0;
            (void)memset(&slot->connection, 0, sizeof(slot->connection));
            return slot;
        }
    }
    return NULL;
}

static void release_slot(http_server_connection_slot_t* slot)
{
    http_server_t* server = slot->server;

    /* Bumping the generation under the lock is what invalidates a
     * deferral ticket still held by a worker thread: its next
     * http_deferral_complete sees the mismatch and gets not_found, instead
     * of writing a response into whatever connection reuses this slot. */
    if (server != NULL)
    {
        (void)pthread_mutex_lock(&server->pending_mutex);
    }
    slot->in_use            = false;
    slot->state             = http_slot_state_idle;
    slot->pending_ready     = false;
    slot->pending_cancelled = false;
    slot->generation++;
    if (server != NULL)
    {
        (void)pthread_mutex_unlock(&server->pending_mutex);
    }

    slot->recv_used         = 0;
    slot->send_used         = 0;
    slot->send_offset       = 0;
    slot->registered_events = 0;

    /* A slot just became free; if the listener was unregistered while all
     * slots were busy, re-arm it. We're on the loop thread so this is
     * safe without any cross-thread signalling. */
    if (server != NULL && !server->listen_registered &&
        server_get_state(server) == http_server_state_running)
    {
        int listen_sd = server->local_endpoint.socket.listen_sd;
        if (listen_sd != -1 &&
            event_loop_register(&server->loop, listen_sd,
                                event_loop_event_read,
                                on_listen_readable, server) == ok)
        {
            server->listen_registered = true;
        }
    }
}

/* ------------------------------------------------------------------------- *
 * Default response factory.
 * ------------------------------------------------------------------------- */
static void prepare_default_response(http_response_t* response, span_t code, span_t reason)
{
    (void)memset(response, 0, sizeof(*response));
    response->http_version  = HTTP_VERSION_1_1;
    response->code          = code;
    response->reason_phrase = reason;
    response->body          = SPAN_EMPTY;
    response->headers.buffer    = SPAN_EMPTY;
    response->headers.used_size = 0;
    response->headers.iterator  = SPAN_EMPTY;
}

static bool client_wants_close(http_request_t* request)
{
    span_t value;
    if (http_headers_find(&request->headers, HDR_CONNECTION, &value) == HL_RESULT_OK)
    {
        /* The connection option is a case-insensitive token (RFC 7230
         * 6.1), and "Close" is as valid as "close". Comparing it
         * byte-exact left the slot open against a client that had said
         * it was leaving, which is a slot leaked per request. */
        if (span_icompare(value, HDR_VAL_CLOSE, true) == 0)
        {
            return true;
        }
    }
    if (span_compare(request->http_version, HTTP_VERSION_1_1) != 0)
    {
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- *
 * Buffer-backed stream used to serialise the response into the slot's
 * send buffer with no allocations.
 *
 * The stream_t inline helpers pass `inner_stream` (cast to struct stream*)
 * as the first argument to the callbacks. We stash a buffer_stream_t
 * pointer in inner_stream and recover it inside the callbacks.
 * ------------------------------------------------------------------------- */
typedef struct buffer_stream
{
    uint8_t* ptr;
    uint32_t capacity;
    uint32_t used;
    bool     overflowed;
} buffer_stream_t;

static result_t bs_open (struct stream* inner) { (void)inner; return ok; }
static result_t bs_close(struct stream* inner) { (void)inner; return ok; }
static result_t bs_read (struct stream* inner, span_t b, span_t* r, span_t* m)
{ (void)inner; (void)b; (void)r; (void)m; return error; }

static result_t bs_write(struct stream* inner, span_t data, span_t* remainder)
{
    buffer_stream_t* bs = (buffer_stream_t*)inner;
    uint32_t n = span_get_size(data);
    if (bs->used + n > bs->capacity)
    {
        bs->overflowed = true;
        if (remainder != NULL) *remainder = data;
        return error;
    }
    (void)memcpy(bs->ptr + bs->used, span_get_ptr(data), n);
    bs->used += n;
    if (remainder != NULL) *remainder = SPAN_EMPTY;
    return ok;
}

/* ------------------------------------------------------------------------- *
 * Slot lifecycle helpers.
 * ------------------------------------------------------------------------- */
static void slot_arm(http_server_connection_slot_t* slot, uint32_t events)
{
    int fd = slot->connection.socket.sd;
    if (fd < 0) return;

    if (slot->registered_events == 0)
    {
        if (event_loop_register(&slot->server->loop, fd, events,
                                on_connection_io, slot) == ok)
        {
            slot->registered_events = events;
        }
    }
    else if (slot->registered_events != events)
    {
        if (event_loop_modify(&slot->server->loop, fd, events) == ok)
        {
            slot->registered_events = events;
        }
    }
}

/* Invoke the streaming finalizer (if any) exactly once, releasing handler
 * resources tied to a streamed body. Safe to call on any teardown path. */
static void stream_finalize(http_server_connection_slot_t* slot)
{
    if (slot->stream_active)
    {
        slot->stream_active = false;   /* clear first: finalizer must run once */
        if (slot->stream_finalizer != NULL)
        {
            slot->stream_finalizer(slot->stream_ctx);
        }
        slot->stream_eof       = false;
        slot->stream_provider  = NULL;
        slot->stream_finalizer = NULL;
        slot->stream_ctx       = NULL;
    }
}

static void slot_close(http_server_connection_slot_t* slot)
{
    stream_finalize(slot);

    int fd = slot->connection.socket.sd;
    if (slot->registered_events != 0 && fd >= 0)
    {
        (void)event_loop_unregister(&slot->server->loop, fd);
        slot->registered_events = 0;
    }
    /* The non-blocking flow never initialised connection->stream, so we
     * must close the socket directly rather than via http_connection_close
     * (which dereferences NULL stream callbacks). */
    (void)socket_deinit(&slot->connection.socket);
    slot->connection.endpoint = NULL;
    release_slot(slot);
}

/* Translate socket io_want into event_loop event mask. Defaults to read
 * if unknown so we don't accidentally block forever. */
static uint32_t want_to_events(uint32_t want)
{
    uint32_t e = 0;
    if (want & socket_io_want_read)  e |= event_loop_event_read;
    if (want & socket_io_want_write) e |= event_loop_event_write;
    if (e == 0) e = event_loop_event_read;
    return e;
}

/* ------------------------------------------------------------------------- *
 * State: handshaking.
 * ------------------------------------------------------------------------- */
static void slot_drive_handshake(http_server_connection_slot_t* slot)
{
    result_t r = socket_handshake_nb(&slot->connection.socket);
    if (r == ok)
    {
        slot->state = http_slot_state_receiving;
        http_request_parser_init(&slot->parser);
        slot_arm(slot, event_loop_event_read);
        return;
    }
    if (r == try_again)
    {
        slot_arm(slot,
                 want_to_events(socket_get_io_want(&slot->connection.socket)));
        return;
    }
    slot_close(slot);
}

/* ------------------------------------------------------------------------- *
 * State: receiving.
 *
 * Reads bytes from the TLS socket via SSL_read into the recv buffer
 * (offset by recv_used). Feeds the incremental parser. Once the full
 * request is parsed, dispatches to the handler and transitions to sending.
 * ------------------------------------------------------------------------- */
static void slot_drive_receive(http_server_connection_slot_t* slot)
{
    socket_t* sock = &slot->connection.socket;

    for (;;)
    {
        if (slot->recv_used >= slot->recv_buffer_size)
        {
            /* The request does not fit this slot's receive buffer.
             * Dropping the connection (the previous behaviour) surfaces in
             * a browser as an unexplained network error with nothing to
             * diagnose, so say what actually happened, then close. */
            log_error("recv buffer full (%u bytes), replying 413", slot->recv_used);
            slot->client_wants_close = true;
            respond_with_status(slot, HTTP_CODE_413, HTTP_REASON_PHRASE_413);
            return;
        }

        uint32_t got = 0;
        result_t rr = socket_read_nb(sock,
                                     slot->recv_buffer_ptr + slot->recv_used,
                                     slot->recv_buffer_size - slot->recv_used,
                                     &got);
        if (rr == try_again)
        {
            uint32_t want = socket_get_io_want(sock);
            uint32_t events = (want & socket_io_want_write)
                                ? event_loop_event_write
                                : event_loop_event_read;
            slot_arm(slot, events);
            return;
        }
        if (rr != ok)
        {
            /* end_of_data or error -- close the connection. */
            slot_close(slot);
            return;
        }

        slot->recv_used += got;

        span_t buf = span_init(slot->recv_buffer_ptr, slot->recv_used);
        result_t pr = http_request_parser_feed(&slot->parser, buf);

        if (pr == ok)
        {
            http_request_t* req = http_request_parser_get_request(&slot->parser);
            slot->client_wants_close = client_wants_close(req);
            run_handler_and_serialize(slot, req);
            return;
        }
        if (pr == try_again)
        {
            /* Need more bytes -- the TLS record layer or kernel buffer
             * may already have more, so retry the non-blocking read. */
            continue;
        }
        log_error("request parse error");
        slot_close(slot);
        return;
    }
}

/* ------------------------------------------------------------------------- *
 * Handler dispatch + response serialisation into the send buffer.
 * ------------------------------------------------------------------------- */
static void run_handler_and_serialize(http_server_connection_slot_t* slot,
                                      http_request_t*               request)
{
    http_server_t*  server = slot->server;
    http_response_t response;
    prepare_default_response(&response, HTTP_CODE_404, HTTP_REASON_PHRASE_404);

    bool                   method_matched = false;
    bool                   route_matched  = false;
    http_handler_outcome_t outcome        = http_handler_respond;

    for (uint32_t i = 0; i < server->route_count; i++)
    {
        http_route_t* route = &server->storage->routes[i];
        if (span_compare(route->method, request->method) != 0)
        {
            continue;
        }
        method_matched = true;

        span_t   path_matches[5];
        uint16_t number_of_matches = 0;

        result_t mr = span_regex_match(&route->compiled_path,
                                       request->path,
                                       path_matches,
                                       (uint16_t)sizeofarray(path_matches),
                                       &number_of_matches);
        if (mr == ok)
        {
            prepare_default_response(&response, HTTP_CODE_200, HTTP_REASON_PHRASE_200);

            http_exchange_t exchange;
            exchange.slot            = slot;
            exchange.request         = request;
            exchange.captures        = path_matches;
            exchange.capture_count   = number_of_matches;
            exchange.response        = &response;
            exchange.defer_requested = false;

            outcome = route->handler(&exchange, route->user_context);

            if (outcome != http_handler_defer && exchange.defer_requested)
            {
                /* A ticket was taken but the handler did not return
                 * http_handler_defer, so whatever the worker eventually
                 * posts will be rejected as stale. Almost always a missing
                 * `return http_handler_defer`, and silent otherwise. */
                log_error("handler took a deferral but returned outcome %d",
                          (int)outcome);
            }

            route_matched = true;
            break;
        }
    }

    if (route_matched)
    {
        if (outcome == http_handler_close)
        {
            /* The handler wants the connection gone and has nothing to
             * say. Previously inexpressible: a handler had to invent some
             * response. */
            slot_close(slot);
            return;
        }

        if (outcome == http_handler_defer)
        {
            /* Nothing to serialise yet. Park the connection; read interest
             * stays armed so a peer that goes away is noticed immediately
             * rather than at the deferral deadline. */
            (void)pthread_mutex_lock(&server->pending_mutex);
            slot->pending_ready       = false;
            slot->pending_cancelled   = false;
            slot->pending_deadline_ms = now_ms() + (uint64_t)server->pending_timeout_ms;
            slot->state               = http_slot_state_pending;
            (void)pthread_mutex_unlock(&server->pending_mutex);
            slot_arm(slot, event_loop_event_read);
            return;
        }
    }

    if (!route_matched && method_matched)
    {
        prepare_default_response(&response, HTTP_CODE_404, HTTP_REASON_PHRASE_404);
    }
    else if (!method_matched && server->route_count > 0)
    {
        prepare_default_response(&response, HTTP_CODE_405, HTTP_REASON_PHRASE_405);
    }

    /* Everything the server answers on its own behalf has an empty body
     * and, until now, no headers whatsoever -- which is not "a response
     * with no body" on the wire, it is a response whose body ends when
     * the connection does (RFC 7230 3.3.3). A keep-alive client therefore
     * sat waiting on a 404 until it timed out. A handler's response is
     * left alone: it owns its own header buffer and its own framing. */
    if (!route_matched)
    {
        response.headers.buffer    = span_init(slot->default_headers,
                                               (uint32_t)sizeof(slot->default_headers));
        response.headers.used_size = 0;
        response.headers.iterator  = SPAN_EMPTY;
        (void)http_headers_add(&response.headers, HTTP_HEADER_CONTENT_LENGTH, HDR_VAL_ZERO);
    }

    serialize_and_send(slot, &response);
}

/* Serialise `response` into the slot's send buffer and start writing.
 * Shared by the immediate path, the deferred-completion path, and the
 * responses the server generates on its own behalf. */
static void serialize_and_send(http_server_connection_slot_t* slot,
                               http_response_t*              response)
{
    /* If the handler supplied a streaming body, remember it on the slot and
     * serialise only the head (status line + headers). The body is then
     * pulled from the provider as the send buffer drains. The handler is
     * responsible for setting a correct Content-Length header. */
    if (response->body_provider != NULL)
    {
        slot->stream_active    = true;
        slot->stream_eof       = false;
        slot->stream_provider  = response->body_provider;
        slot->stream_finalizer = response->body_finalizer;
        slot->stream_ctx       = response->body_provider_context;
        response->body         = SPAN_EMPTY;
    }

    /* Serialise into the send buffer. */
    buffer_stream_t bs = {
        .ptr        = slot->send_buffer_ptr,
        .capacity   = slot->send_buffer_size,
        .used       = 0,
        .overflowed = false,
    };
    stream_t st = {
        .open         = bs_open,
        .close        = bs_close,
        .write        = bs_write,
        .read         = bs_read,
        .inner_stream = (void*)&bs,
    };

    if (is_error(http_response_serialize_to(response, &st)) || bs.overflowed)
    {
        log_error("response serialise failed (overflow=%d)", bs.overflowed);
        slot_close(slot);
        return;
    }

    slot->send_used   = bs.used;
    slot->send_offset = 0;
    slot->state       = http_slot_state_sending;
    slot_arm(slot, event_loop_event_write);
}

/* Answer with a bare status line the server produces itself (no handler
 * involved), borrowing the slot's small default header buffer so the
 * response is properly framed rather than body-ends-at-close. */
static void respond_with_status(http_server_connection_slot_t* slot,
                                span_t code, span_t reason)
{
    http_response_t response;
    prepare_default_response(&response, code, reason);
    response.headers.buffer    = span_init(slot->default_headers,
                                           (uint32_t)sizeof(slot->default_headers));
    response.headers.used_size = 0;
    response.headers.iterator  = SPAN_EMPTY;
    (void)http_headers_add(&response.headers, HTTP_HEADER_CONTENT_LENGTH, HDR_VAL_ZERO);
    serialize_and_send(slot, &response);
}

/* ------------------------------------------------------------------------- *
 * State: sending.
 * ------------------------------------------------------------------------- */
static void slot_finish_send(http_server_connection_slot_t* slot)
{
    if (slot->client_wants_close)
    {
        slot_close(slot);
        return;
    }

    /* Keep-alive: shift any pipelined trailing bytes down to offset 0 and
     * re-init the parser. */
    uint32_t consumed = http_request_parser_get_consumed(&slot->parser);
    if (consumed > slot->recv_used) consumed = slot->recv_used;
    uint32_t leftover = slot->recv_used - consumed;
    if (leftover > 0 && consumed > 0)
    {
        (void)memmove(slot->recv_buffer_ptr,
                      slot->recv_buffer_ptr + consumed,
                      leftover);
    }
    slot->recv_used   = leftover;
    slot->send_used   = 0;
    slot->send_offset = 0;
    slot->state       = http_slot_state_receiving;
    http_request_parser_init(&slot->parser);

    if (slot->recv_used > 0)
    {
        span_t buf = span_init(slot->recv_buffer_ptr, slot->recv_used);
        result_t pr = http_request_parser_feed(&slot->parser, buf);
        if (pr == ok)
        {
            http_request_t* req = http_request_parser_get_request(&slot->parser);
            slot->client_wants_close = client_wants_close(req);
            run_handler_and_serialize(slot, req);
            return;
        }
        if (is_error(pr))
        {
            slot_close(slot);
            return;
        }
    }

    slot_arm(slot, event_loop_event_read);
}

static void slot_drive_send(http_server_connection_slot_t* slot)
{
    for (;;)
    {
        while (slot->send_offset < slot->send_used)
        {
            span_t to_send = span_init(slot->send_buffer_ptr + slot->send_offset,
                                       slot->send_used - slot->send_offset);
            uint32_t written = 0;
            result_t r = socket_write_nb(&slot->connection.socket, to_send, &written);
            slot->send_offset += written;

            if (r == ok)
            {
                break;
            }
            if (r == try_again)
            {
                slot_arm(slot,
                         want_to_events(socket_get_io_want(&slot->connection.socket)));
                return;
            }
            slot_close(slot);
            return;
        }

        /* Send buffer drained. If a streaming body is active, pull the next
         * chunk from the provider and keep sending; otherwise we're done. */
        if (slot->stream_active && !slot->stream_eof)
        {
            uint32_t n = slot->stream_provider(slot->stream_ctx,
                                               slot->send_buffer_ptr,
                                               slot->send_buffer_size);
            if (n > 0)
            {
                slot->send_used   = n;
                slot->send_offset = 0;
                continue;
            }
            slot->stream_eof = true;
            stream_finalize(slot);
        }
        break;
    }

    slot_finish_send(slot);
}

/* ------------------------------------------------------------------------- *
 * Per-connection event dispatcher.
 * ------------------------------------------------------------------------- */
/* Parked awaiting a deferred response. Read interest stays armed purely to
 * notice the peer leaving, so a closed browser tab frees its slot at once
 * rather than lingering until the deferral deadline. */
static void slot_probe_pending(http_server_connection_slot_t* slot)
{
    if (slot->recv_used >= slot->recv_buffer_size)
    {
        /* Nowhere to put more bytes. Not fatal -- wait for the response
         * or the deadline rather than dropping a peer that is still
         * there. */
        return;
    }

    uint32_t got = 0;
    result_t rr  = socket_read_nb(&slot->connection.socket,
                                  slot->recv_buffer_ptr + slot->recv_used,
                                  slot->recv_buffer_size - slot->recv_used,
                                  &got);
    if (rr == try_again)
    {
        return;
    }
    if (rr != ok)
    {
        /* end_of_data or error: the peer is gone. Closing bumps the slot
         * generation, so a completion still in flight on a worker thread
         * lands as not_found instead of on a recycled connection. */
        slot_close(slot);
        return;
    }

    /* Pipelined bytes belonging to a later request. Keep them buffered;
     * they are parsed once the deferred response has been sent. */
    slot->recv_used += got;
}

static void slot_advance(http_server_connection_slot_t* slot)
{
    switch (slot->state)
    {
        case http_slot_state_handshaking: slot_drive_handshake(slot); break;
        case http_slot_state_receiving:   slot_drive_receive(slot);   break;
        case http_slot_state_pending:     slot_probe_pending(slot);   break;
        case http_slot_state_sending:     slot_drive_send(slot);      break;
        default:                          slot_close(slot);           break;
    }
}

static void on_connection_io(int fd, uint32_t events, void* user)
{
    (void)fd;
    http_server_connection_slot_t* slot = (http_server_connection_slot_t*)user;
    if (events & event_loop_event_error)
    {
        slot_close(slot);
        return;
    }
    slot_advance(slot);
}

/* ------------------------------------------------------------------------- *
 * Deferred responses.
 * ------------------------------------------------------------------------- */

/* Serialise responses handed back by worker threads. Runs on the loop
 * thread after every iteration, which is what keeps the send buffer
 * single-threaded even though completions arrive from anywhere. */
static void flush_pending_responses(http_server_t* server)
{
    for (uint32_t i = 0; i < server->storage->slot_count; i++)
    {
        http_server_connection_slot_t* slot = &server->storage->slots[i];

        http_response_t response;
        bool            have      = false;
        bool            cancelled = false;

        (void)pthread_mutex_lock(&server->pending_mutex);
        if (slot->in_use &&
            slot->state == http_slot_state_pending &&
            (slot->pending_ready || slot->pending_cancelled))
        {
            cancelled = slot->pending_cancelled;
            if (!cancelled)
            {
                response = slot->pending_response;
            }
            slot->pending_ready     = false;
            slot->pending_cancelled = false;
            /* Claim the slot before dropping the lock so a second
             * completion cannot race in behind this one. */
            slot->state             = http_slot_state_sending;
            have                    = true;
        }
        (void)pthread_mutex_unlock(&server->pending_mutex);

        if (have)
        {
            if (cancelled)
            {
                /* The application knows no answer is coming. Saying so now
                 * beats making the client wait out the full deferral
                 * timeout for a reply that will never arrive. */
                slot->client_wants_close = true;
                respond_with_status(slot, HTTP_CODE_503, HTTP_REASON_PHRASE_503);
            }
            else
            {
                serialize_and_send(slot, &response);
            }
        }
    }
}

/* Enforce the deferral ceiling. An application that loses track of a
 * handle must not strand a connection slot for the life of the process. */
static void sweep_pending_deadlines(http_server_t* server)
{
    uint64_t now = now_ms();

    for (uint32_t i = 0; i < server->storage->slot_count; i++)
    {
        http_server_connection_slot_t* slot = &server->storage->slots[i];
        bool expired = false;

        (void)pthread_mutex_lock(&server->pending_mutex);
        if (slot->in_use &&
            slot->state == http_slot_state_pending &&
            !slot->pending_ready &&
            !slot->pending_cancelled &&
            now >= slot->pending_deadline_ms)
        {
            slot->state = http_slot_state_sending;
            expired     = true;
        }
        (void)pthread_mutex_unlock(&server->pending_mutex);

        if (expired)
        {
            slot->client_wants_close = true;
            respond_with_status(slot, HTTP_CODE_504, HTTP_REASON_PHRASE_504);
        }
    }
}

/* How long the loop may sleep. With nothing parked we block indefinitely
 * and rely on event_loop_wake() from a completion; with something parked
 * we must surface in time to enforce its deadline. */
static int next_tick_ms(http_server_t* server)
{
    uint64_t soonest = 0;
    bool     any     = false;

    (void)pthread_mutex_lock(&server->pending_mutex);
    for (uint32_t i = 0; i < server->storage->slot_count; i++)
    {
        http_server_connection_slot_t* slot = &server->storage->slots[i];
        if (slot->in_use && slot->state == http_slot_state_pending)
        {
            if (!any || slot->pending_deadline_ms < soonest)
            {
                soonest = slot->pending_deadline_ms;
                any     = true;
            }
        }
    }
    (void)pthread_mutex_unlock(&server->pending_mutex);

    if (!any)
    {
        return -1;
    }

    uint64_t now = now_ms();
    if (soonest <= now)
    {
        return 0;
    }

    uint64_t delta = soonest - now;
    /* Cap the sleep so shutdown latency stays bounded on backends whose
     * wake is advisory rather than immediate. */
    if (delta > 1000)
    {
        delta = 1000;
    }
    return (int)delta;
}

http_deferral_t http_exchange_defer(http_exchange_t* exchange)
{
    http_deferral_t deferral;
    (void)memset(&deferral, 0, sizeof(deferral));

    if (exchange == NULL || exchange->slot == NULL)
    {
        return deferral;
    }

    http_server_connection_slot_t* slot = exchange->slot;

    /* Recorded so the server can point out a handler that took a ticket
     * and then forgot to return http_handler_defer -- otherwise the
     * eventual completion is silently rejected as stale. */
    exchange->defer_requested = true;

    deferral.server     = slot->server;
    deferral.slot       = slot;
    deferral.generation = slot->generation;
    return deferral;
}

/* Validate a ticket without dereferencing anything it claims to point at.
 *
 * Done in uintptr_t rather than by comparing pointers: relational
 * comparison of pointers that are not into the same array object is
 * undefined behaviour in C, so `slot < first || slot >= limit` would be
 * relying on exactly the thing it is trying to defend against. The modulo
 * check additionally rejects a pointer that lands inside the array but not
 * on a slot boundary. */
static http_server_connection_slot_t* deferral_slot(http_deferral_t deferral)
{
    if (deferral.server == NULL || deferral.slot == NULL ||
        deferral.server->storage == NULL)
    {
        return NULL;
    }

    uintptr_t base   = (uintptr_t)(void*)deferral.server->storage->slots;
    uintptr_t probe  = (uintptr_t)(void*)deferral.slot;
    size_t    stride = sizeof(http_server_connection_slot_t);
    uintptr_t extent = (uintptr_t)deferral.server->storage->slot_count * (uintptr_t)stride;

    if (probe < base || probe >= base + extent ||
        ((probe - base) % (uintptr_t)stride) != 0)
    {
        return NULL;
    }

    return (http_server_connection_slot_t*)deferral.slot;
}

static result_t deferral_post(http_deferral_t  deferral,
                              http_response_t* response,
                              bool             cancelled)
{
    http_server_connection_slot_t* slot = deferral_slot(deferral);
    if (slot == NULL)
    {
        return invalid_argument;
    }

    http_server_t* server = deferral.server;
    result_t       result;

    (void)pthread_mutex_lock(&server->pending_mutex);
    if (!slot->in_use ||
        slot->state != http_slot_state_pending ||
        slot->generation != deferral.generation)
    {
        /* Peer disconnected, deadline fired, or the slot was recycled.
         * Routine in normal operation -- the caller drops the ticket. */
        result = not_found;
    }
    else if (slot->pending_ready || slot->pending_cancelled)
    {
        result = error;   /* already completed once */
    }
    else
    {
        if (cancelled)
        {
            slot->pending_cancelled = true;
        }
        else
        {
            slot->pending_response = *response;
            slot->pending_ready    = true;
        }
        result = ok;
    }
    (void)pthread_mutex_unlock(&server->pending_mutex);

    if (result == ok)
    {
        /* Pull the loop thread out of its wait so the response goes out
         * now rather than whenever the next socket event happens. */
        (void)event_loop_wake(&server->loop);
    }
    return result;
}

result_t http_deferral_complete(http_deferral_t deferral, http_response_t* response)
{
    if (response == NULL)
    {
        return invalid_argument;
    }
    return deferral_post(deferral, response, false);
}

result_t http_deferral_cancel(http_deferral_t deferral)
{
    return deferral_post(deferral, NULL, true);
}

/* ------------------------------------------------------------------------- *
 * Public API.
 * ------------------------------------------------------------------------- */
result_t http_server_init(http_server_t* server, http_server_config_t* config, http_server_storage_t* storage)
{
    if (server == NULL || config == NULL || storage == NULL ||
        storage->slots == NULL || storage->routes == NULL ||
        storage->slot_count == 0 || storage->route_count == 0)
    {
        return invalid_argument;
    }

    (void)memset(server, 0, sizeof(http_server_t));

    if (pthread_mutex_init(&server->state_mutex, NULL) != 0)
    {
        return error;
    }
    if (pthread_cond_init(&server->state_cond, NULL) != 0)
    {
        (void)pthread_mutex_destroy(&server->state_mutex);
        return error;
    }
    if (pthread_mutex_init(&server->pending_mutex, NULL) != 0)
    {
        (void)pthread_cond_destroy(&server->state_cond);
        (void)pthread_mutex_destroy(&server->state_mutex);
        return error;
    }

    server->storage     = storage;
    server->route_count = 0;
    server->pending_timeout_ms = (config->deferred_timeout_ms != 0)
                                    ? config->deferred_timeout_ms
                                    : HTTP_SERVER_DEFAULT_DEFERRED_TIMEOUT_MS;

    for (uint32_t i = 0; i < storage->slot_count; i++)
    {
        storage->slots[i].in_use            = false;
        storage->slots[i].server            = NULL;
        storage->slots[i].state             = http_slot_state_idle;
        storage->slots[i].registered_events = 0;
        /* Caller-supplied storage is not guaranteed to be zeroed, and a
         * garbage generation would make deferral tickets unverifiable. */
        storage->slots[i].generation          = 0;
        storage->slots[i].pending_ready       = false;
        storage->slots[i].pending_cancelled   = false;
        storage->slots[i].pending_deadline_ms = 0;
    }
    for (uint32_t i = 0; i < storage->route_count; i++)
    {
        storage->routes[i].in_use = false;
    }

    http_endpoint_config_t* lc = &server->local_endpoint_config;
    lc->role                     = http_endpoint_server;
    lc->local.port               = config->port;
    lc->tls.enable               = config->tls.enable;
    lc->tls.certificate_file     = config->tls.certificate_file;
    lc->tls.private_key_file     = config->tls.private_key_file;
    server->state_changed_cb  = config->on_state_changed;
    server->state_changed_ctx = config->on_state_changed_context;

    server->state = http_server_state_initialized;
    return ok;
}

result_t http_server_deinit(http_server_t* server)
{
    if (server == NULL)
    {
        return invalid_argument;
    }

    if (server->storage != NULL)
    {
        for (uint32_t i = 0; i < server->route_count; i++)
        {
            span_regex_free(&server->storage->routes[i].compiled_path);
            server->storage->routes[i].in_use = false;
        }
    }
    server->route_count = 0;

    (void)pthread_mutex_destroy(&server->pending_mutex);
    (void)pthread_cond_destroy(&server->state_cond);
    (void)pthread_mutex_destroy(&server->state_mutex);
    return ok;
}

result_t http_server_add_route(http_server_t* server, span_t method, span_t path, http_request_handler_t handler, void* user_context)
{
    if (server == NULL || span_is_empty(method) || span_is_empty(path) || handler == NULL)
    {
        return invalid_argument;
    }
    if (server->route_count == server->storage->route_count)
    {
        return insufficient_size;
    }

    http_route_t* route = &server->storage->routes[server->route_count];
    route->method       = method;
    route->path         = path;
    route->handler      = handler;
    route->user_context = user_context;
    route->in_use       = true;

    if (span_regex_compile(&route->compiled_path, path) != ok)
    {
        return error;
    }
    server->route_count++;
    return ok;
}

result_t http_server_stop(http_server_t* server)
{
    if (server == NULL)
    {
        return invalid_argument;
    }

    /* Wait (bounded) for the run loop to publish either `running` (the
     * common case: stop was called after run started up) or `stopped`
     * (run already exited on its own). Both states are terminal as far
     * as our wait is concerned. The 1s deadline is a safety net for the
     * pathological case where http_server_stop is called before
     * http_server_run was ever spawned -- we still proceed to mark the
     * server stopping and tell the loop to break out, so a subsequent
     * run() will observe `stopping` and bail out cleanly. */
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;

    (void)pthread_mutex_lock(&server->state_mutex);
    while (server->state != http_server_state_running &&
           server->state != http_server_state_stopped)
    {
        if (pthread_cond_timedwait(&server->state_cond,
                                   &server->state_mutex,
                                   &deadline) != 0)
        {
            break; /* timeout or error -- proceed anyway */
        }
    }
    bool already_stopped = (server->state == http_server_state_stopped);
    (void)pthread_mutex_unlock(&server->state_mutex);

    if (already_stopped)
    {
        return ok;
    }

    server_set_state(server, http_server_state_stopping);
    (void)event_loop_stop(&server->loop);
    return ok;
}

/* ------------------------------------------------------------------------- *
 * Accept callback.
 * ------------------------------------------------------------------------- */
static void on_listen_readable(int fd, uint32_t events, void* user)
{
    (void)events;
    http_server_t* server = (http_server_t*)user;
    if (server_get_state(server) != http_server_state_running)
    {
        return;
    }

    http_server_connection_slot_t* slot = acquire_slot(server);
    if (slot == NULL)
    {
        /* All slots busy. Stop listening until a slot frees. */
        (void)event_loop_unregister(&server->loop, fd);
        server->listen_registered = false;
        return;
    }

    result_t ar = socket_accept_nb(&server->local_endpoint.socket,
                                   &slot->connection.socket);
    if (ar == try_again)
    {
        release_slot(slot);
        return;
    }
    if (is_error(ar))
    {
        release_slot(slot);
        return;
    }

    slot->connection.endpoint = &server->local_endpoint;
    slot->state               = http_slot_state_handshaking;
    /* Drive the handshake immediately. SSL_do_handshake will report
     * WANT_READ/WANT_WRITE if it cannot complete synchronously. */
    slot_drive_handshake(slot);
}

result_t http_server_run(http_server_t* server)
{
    if (server == NULL)
    {
        return invalid_argument;
    }
    if (server_get_state(server) == http_server_state_running)
    {
        return error;
    }

    (void)task_platform_init();

    if (is_error(http_endpoint_init(&server->local_endpoint, &server->local_endpoint_config)))
    {
        return error;
    }

    /* Listening socket must be non-blocking so accept_nb works. */
    if (socket_set_nonblocking(server->local_endpoint.socket.listen_sd) != ok)
    {
        (void)http_endpoint_deinit(&server->local_endpoint);
        return error;
    }

    if (is_error(event_loop_init(&server->loop)))
    {
        (void)http_endpoint_deinit(&server->local_endpoint);
        return error;
    }

    int listen_sd = server->local_endpoint.socket.listen_sd;
    if (event_loop_register(&server->loop, listen_sd,
                            event_loop_event_read,
                            on_listen_readable, server) != ok)
    {
        (void)event_loop_deinit(&server->loop);
        (void)http_endpoint_deinit(&server->local_endpoint);
        return error;
    }
    server->listen_registered = true;

    /* Listening socket is bound and registered with the event loop, so
     * connect()s from clients will be queued by the kernel from this
     * point on. Publish the running state (which fires the
     * on_state_changed callback) BEFORE entering the run loop. We only
     * promote from `initialized` to `running`; any other state means
     * stop() was already called and we must not regress it. */
    bool publish_running = false;
    (void)pthread_mutex_lock(&server->state_mutex);
    if (server->state == http_server_state_initialized)
    {
        publish_running = true;
    }
    (void)pthread_mutex_unlock(&server->state_mutex);
    if (publish_running)
    {
        server_set_state(server, http_server_state_running);
    }

    /* Deliberately not event_loop_run(): deferred responses need a hook
     * after each iteration, to flush completions posted by worker threads
     * and to enforce deferral deadlines. event_loop_run offers no such
     * hook; run_once is exactly that hook. Stop is observed through the
     * server's own published state, which http_server_stop sets before it
     * stops the loop, so no event_loop internals are touched here. */
    result_t result = ok;
    while (server_get_state(server) == http_server_state_running)
    {
        result = event_loop_run_once(&server->loop, next_tick_ms(server));
        if (is_error(result))
        {
            break;
        }
        flush_pending_responses(server);
        sweep_pending_deadlines(server);
    }

    /* Tear down: close any in-flight connections and unregister fds. */
    for (uint32_t i = 0; i < server->storage->slot_count; i++)
    {
        http_server_connection_slot_t* slot = &server->storage->slots[i];
        if (slot->in_use)
        {
            stream_finalize(slot);
            int sd = slot->connection.socket.sd;
            if (slot->registered_events != 0 && sd >= 0)
            {
                (void)event_loop_unregister(&server->loop, sd);
                slot->registered_events = 0;
            }
            (void)socket_deinit(&slot->connection.socket);
            slot->connection.endpoint = NULL;
            (void)pthread_mutex_lock(&server->pending_mutex);
            slot->pending_ready     = false;
            slot->pending_cancelled = false;
            slot->generation++;
            slot->in_use = false;
            (void)pthread_mutex_unlock(&server->pending_mutex);
        }
    }

    if (server->listen_registered)
    {
        (void)event_loop_unregister(&server->loop, listen_sd);
        server->listen_registered = false;
    }
    (void)event_loop_deinit(&server->loop);
    (void)http_endpoint_deinit(&server->local_endpoint);
    server_set_state(server, http_server_state_stopped);
    return result;
}

static result_t internal_http_server_run_async(void* state, task_t* self)
{
    (void)self;
    return http_server_run((http_server_t*)state);
}

task_t* http_server_run_async(http_server_t* server)
{
    if (server == NULL)
    {
        return NULL;
    }
    return task_run(internal_http_server_run_async, server);
}
