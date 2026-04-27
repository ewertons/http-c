#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/eventfd.h>

#include "common_lib_c.h"

#include "http_server.h"
#include "http_methods.h"
#include "http_endpoint.h"
#include "http_connection.h"
#include "http_codes.h"
#include "http_versions.h"
#include "http_headers.h"
#include "common.h"
#include "socket_stream.h"
#include "event_loop.h"

/* ------------------------------------------------------------------------- *
 * Internal helpers.
 * ------------------------------------------------------------------------- */

static const span_t HDR_CONNECTION = span_from_str_literal("Connection");
static const span_t HDR_VAL_CLOSE  = span_from_str_literal("close");

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
    (void)pthread_mutex_unlock(&server->state_mutex);
}

/* Returns NULL if no slot is available. The slot is owned by the caller
 * until #release_slot is invoked. */
static http_server_connection_slot_t* acquire_slot(http_server_t* server)
{
    (void)pthread_mutex_lock(&server->state_mutex);

    http_server_connection_slot_t* slot = NULL;
    for (uint32_t i = 0; i < server->storage->slot_count; i++)
    {
        if (!server->storage->slots[i].in_use)
        {
            server->storage->slots[i].in_use = true;
            server->storage->slots[i].server = server;
            server->storage->slots[i].task   = NULL;
            slot = &server->storage->slots[i];
            break;
        }
    }

    (void)pthread_mutex_unlock(&server->state_mutex);
    return slot;
}

static void release_slot(http_server_connection_slot_t* slot)
{
    http_server_t* server = slot->server;
    (void)pthread_mutex_lock(&server->state_mutex);
    slot->in_use = false;
    slot->task   = NULL;
    (void)pthread_mutex_unlock(&server->state_mutex);

    /* Notify the accept loop that a slot has freed up. The accept callback
     * may have deregistered the listen fd if all slots were busy; this
     * eventfd write wakes the loop so it can re-arm the listener. */
    if (server->slot_freed_fd != -1)
    {
        uint64_t v = 1;
        ssize_t  n = write(server->slot_freed_fd, &v, sizeof(v));
        (void)n;
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
        if (span_compare(value, HDR_VAL_CLOSE) == 0)
        {
            return true;
        }
    }
    /* HTTP/1.0 defaults to close. */
    if (span_compare(request->http_version, HTTP_VERSION_1_1) != 0)
    {
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- *
 * Per-connection worker.
 * ------------------------------------------------------------------------- */
static result_t connection_worker(void* state, task_t* self)
{
    http_server_connection_slot_t* slot   = (http_server_connection_slot_t*)state;
    http_server_t*                 server = slot->server;
    http_connection_t*             conn   = &slot->connection;

    span_t buffer = span_init(slot->buffer_ptr, slot->buffer_size);

    bool keep_alive = true;
    while (keep_alive && !task_is_cancellation_requested(self) &&
           server_get_state(server) == http_server_state_running)
    {
        http_request_t  request;
        http_response_t response;

        result_t r = http_connection_receive_request(conn, buffer, &request, NULL);

        if (is_error(r))
        {
            /* Connection broken or closed by peer. */
            break;
        }

        /* Default to 404 unless a route claims it. */
        prepare_default_response(&response, HTTP_CODE_404, HTTP_REASON_PHRASE_404);

        bool method_matched = false;
        bool route_matched  = false;

        for (uint32_t i = 0; i < server->route_count; i++)
        {
            if (span_compare(server->storage->routes[i].method, request.method) != 0)
            {
                continue;
            }
            method_matched = true;

            span_t   path_matches[5];
            uint16_t number_of_matches = 0;

            result_t mr = span_regex_match(&server->storage->routes[i].compiled_path,
                                           request.path,
                                           path_matches,
                                           (uint16_t)sizeofarray(path_matches),
                                           &number_of_matches);

            if (mr == ok)
            {
                /* Pre-fill a 200 OK; handler may override. */
                prepare_default_response(&response, HTTP_CODE_200, HTTP_REASON_PHRASE_200);
                server->storage->routes[i].handler(&request, path_matches, number_of_matches,
                                                   &response, server->storage->routes[i].user_context);
                route_matched = true;
                break;
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

        if (is_error(http_connection_send_response(conn, &response)))
        {
            break;
        }

        if (client_wants_close(&request))
        {
            keep_alive = false;
        }
    }

    (void)http_connection_close(conn);
    release_slot(slot);

    return ok;
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

    server->storage     = storage;
    server->route_count = 0;

    /* Reset slot in-use flags so a storage instance can be reused across
     * server lifecycles. Buffer pointers were wired up by the storage
     * provider; we must not clobber them. */
    for (uint32_t i = 0; i < storage->slot_count; i++)
    {
        storage->slots[i].in_use = false;
        storage->slots[i].server = NULL;
        storage->slots[i].task   = NULL;
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

    /* Pre-compile the path pattern once; reused for every incoming request. */
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

    /* Wait for the run loop to be either running or already stopped before
     * attempting to interrupt it - otherwise stop may race with a still
     * pending http_server_run that hasn't initialised the event loop yet. */
    for (int i = 0; i < 1000; i++)
    {
        http_server_state_t s = server_get_state(server);
        if (s == http_server_state_running || s == http_server_state_stopped)
        {
            break;
        }
        task_sleep_ms(1);
    }

    server_set_state(server, http_server_state_stopping);

    /* Wake the event loop. event_loop_stop is safe from any thread. */
    (void)event_loop_stop(&server->loop);
    return ok;
}

/* ------------------------------------------------------------------------- *
 * event_loop callbacks (run on the loop thread).
 * ------------------------------------------------------------------------- */

static void on_listen_readable(int fd, uint32_t events, void* user);

static void on_slot_freed(int fd, uint32_t events, void* user)
{
    (void)events;
    http_server_t* server = (http_server_t*)user;

    /* Drain the eventfd. */
    uint64_t v;
    ssize_t  n;
    do { n = read(fd, &v, sizeof(v)); } while (n == -1 && errno == EINTR);
    (void)n;

    if (server_get_state(server) != http_server_state_running)
    {
        return;
    }

    if (!server->listen_registered)
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
        /* All slots busy. Stop listening for new connections until a slot
         * frees; the on_slot_freed callback will re-arm us. */
        (void)event_loop_unregister(&server->loop, fd);
        server->listen_registered = false;
        return;
    }

    (void)memset(&slot->connection, 0, sizeof(slot->connection));

    /* The listen fd just became readable so accept() will not block. The
     * accepted fd inherits the blocking mode of the listen fd, so the
     * existing blocking-I/O connection_worker keeps working. */
    if (is_error(socket_accept(&server->local_endpoint.socket,
                               &slot->connection.socket)))
    {
        release_slot(slot);
        return;
    }

    if (is_error(socket_stream_initialize(&slot->connection.stream,
                                          &slot->connection.socket)))
    {
        (void)http_connection_close(&slot->connection);
        release_slot(slot);
        return;
    }
    slot->connection.endpoint = &server->local_endpoint;

    slot->task = task_run(connection_worker, slot);
    if (slot->task == NULL)
    {
        (void)http_connection_close(&slot->connection);
        release_slot(slot);
        return;
    }
    /* Fire and forget. The worker holds its own reference. */
    task_release(slot->task);
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

    /* Make sure the global task pool is up. Idempotent. */
    (void)task_platform_init();

    if (is_error(http_endpoint_init(&server->local_endpoint, &server->local_endpoint_config)))
    {
        return error;
    }

    if (is_error(event_loop_init(&server->loop)))
    {
        (void)http_endpoint_deinit(&server->local_endpoint);
        return error;
    }

    server->slot_freed_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (server->slot_freed_fd == -1)
    {
        (void)event_loop_deinit(&server->loop);
        (void)http_endpoint_deinit(&server->local_endpoint);
        return error;
    }

    if (event_loop_register(&server->loop, server->slot_freed_fd,
                            event_loop_event_read,
                            on_slot_freed, server) != ok)
    {
        close(server->slot_freed_fd);
        server->slot_freed_fd = -1;
        (void)event_loop_deinit(&server->loop);
        (void)http_endpoint_deinit(&server->local_endpoint);
        return error;
    }

    int listen_sd = server->local_endpoint.socket.listen_sd;
    if (event_loop_register(&server->loop, listen_sd,
                            event_loop_event_read,
                            on_listen_readable, server) != ok)
    {
        (void)event_loop_unregister(&server->loop, server->slot_freed_fd);
        close(server->slot_freed_fd);
        server->slot_freed_fd = -1;
        (void)event_loop_deinit(&server->loop);
        (void)http_endpoint_deinit(&server->local_endpoint);
        return error;
    }
    server->listen_registered = true;

    /* Atomically promote initialised → running. If a concurrent stop has
     * already moved us into stopping/stopped, the loop will exit on first
     * iteration. */
    (void)pthread_mutex_lock(&server->state_mutex);
    if (server->state == http_server_state_initialized)
    {
        server->state = http_server_state_running;
    }
    (void)pthread_mutex_unlock(&server->state_mutex);

    /* Run until http_server_stop is invoked from another thread. */
    result_t result = event_loop_run(&server->loop);

    /* Tear down event loop registrations. */
    if (server->listen_registered)
    {
        (void)event_loop_unregister(&server->loop, listen_sd);
        server->listen_registered = false;
    }
    (void)event_loop_unregister(&server->loop, server->slot_freed_fd);
    close(server->slot_freed_fd);
    server->slot_freed_fd = -1;
    (void)event_loop_deinit(&server->loop);

    /* Drain in-flight workers. */
    for (uint32_t i = 0; i < server->storage->slot_count; i++)
    {
        bool busy;
        (void)pthread_mutex_lock(&server->state_mutex);
        busy = server->storage->slots[i].in_use;
        (void)pthread_mutex_unlock(&server->state_mutex);

        while (busy)
        {
            task_sleep_ms(10);
            (void)pthread_mutex_lock(&server->state_mutex);
            busy = server->storage->slots[i].in_use;
            (void)pthread_mutex_unlock(&server->state_mutex);
        }
    }

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
