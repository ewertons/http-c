#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
#include "common.h"

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

    /* Wait for the server to either be running or already stopped before
     * attempting to interrupt - otherwise stop may race with a still-pending
     * http_server_run that hasn't initialized the listening socket yet. */
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

    /* Wake up any blocked accept() in the run loop. shutdown(SHUT_RDWR) is
     * not reliable on Linux for waking blocked accept calls, so we also
     * make a throwaway TCP connection to ourselves: accept() will return,
     * the run loop will observe the stopping state and break out. */
    int fd = server->local_endpoint.socket.listen_sd;
    if (fd != -1)
    {
        (void)shutdown(fd, SHUT_RDWR);

        int wake = socket(AF_INET, SOCK_STREAM, 0);
        if (wake != -1)
        {
            struct sockaddr_in sa;
            (void)memset(&sa, 0, sizeof(sa));
            sa.sin_family      = AF_INET;
            sa.sin_port        = htons(server->local_endpoint_config.local.port);
            sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            (void)connect(wake, (struct sockaddr*)&sa, sizeof(sa));
            (void)close(wake);
        }
    }
    return ok;
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

    /* Atomically promote initialized → running. If a concurrent stop has
     * already moved us into stopping/stopped, drop into the drain path. */
    (void)pthread_mutex_lock(&server->state_mutex);
    if (server->state == http_server_state_initialized)
    {
        server->state = http_server_state_running;
    }
    (void)pthread_mutex_unlock(&server->state_mutex);

    result_t result = ok;

    while (server_get_state(server) == http_server_state_running)
    {
        http_server_connection_slot_t* slot = acquire_slot(server);

        if (slot == NULL)
        {
            /* All worker slots busy. Back off briefly. */
            task_sleep_ms(5);
            continue;
        }

        result_t cr = http_endpoint_wait_for_connection(&server->local_endpoint, &slot->connection);

        if (is_error(cr))
        {
            release_slot(slot);
            if (server_get_state(server) != http_server_state_running)
            {
                break;
            }
            /* Transient accept failure - keep looping. */
            continue;
        }

        slot->task = task_run(connection_worker, slot);
        if (slot->task == NULL)
        {
            /* Could not spawn a worker - close and continue. */
            (void)http_connection_close(&slot->connection);
            release_slot(slot);
            continue;
        }
        /* Fire and forget. The worker holds its own reference and we
         * release the caller's. */
        task_release(slot->task);
    }

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
