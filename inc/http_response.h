#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stdbool.h>

#include "http_headers.h"

#include "span.h" 
#include "niceties.h" 

// https://tools.ietf.org/html/rfc2616#page-100


/**
 * Optional pull-based streaming body.
 *
 * When http_response_t::body_provider is non-NULL the server ignores the
 * inline `body` span and instead repeatedly calls the provider to refill
 * its per-connection send buffer until the provider returns 0 (end of
 * body). This lets a handler stream a payload far larger than the send
 * buffer (e.g. a multi-MB file) without buffering it in RAM.
 *
 * Contract:
 *   - The handler MUST set a correct Content-Length header itself; the
 *     server does not derive it from the (streamed) body.
 *   - The provider fills up to `buffer_size` bytes into `buffer` and
 *     returns the number written. Returning 0 signals end-of-body, so it
 *     must only be returned at true EOF (no transient short reads).
 *   - When the response completes OR the connection is torn down early,
 *     the server invokes body_finalizer exactly once (if non-NULL) so the
 *     handler can release resources (close files, free context).
 */
typedef uint32_t (*http_body_provider_t)(void* context, uint8_t* buffer, uint32_t buffer_size);
typedef void     (*http_body_finalizer_t)(void* context);


/**
 * Opaque ticket identifying a request whose response has been deferred.
 *
 * Safe to copy and to hold across threads. The generation counter is what
 * makes that safe: connection slots are a fixed, recycled resource, so a
 * completion that arrives after its peer disconnected would otherwise be
 * written into whatever connection inherited the slot. The server bumps
 * the generation whenever a slot is released, which turns such a late
 * completion into a clean #not_found instead of a response delivered to
 * the wrong client.
 */
typedef struct http_response_handle
{
    void*    slot;        /* opaque: the server's connection slot */
    uint32_t generation;  /* guards against slot reuse */
} http_response_handle_t;


typedef struct http_response 
{
    span_t http_version;
    span_t code;
    span_t reason_phrase;
    http_headers_t headers;
    span_t body;

    /* Optional streaming body (see typedefs above). Zero-initialised by
     * prepare_default_response, so existing handlers keep using `body`. */
    http_body_provider_t  body_provider;
    http_body_finalizer_t body_finalizer;
    void*                 body_provider_context;

    /* Deferred-response plumbing. `deferral` is populated by the server
     * before it invokes a handler and is opaque to handlers; `deferred`
     * is set by #http_response_defer. Both are zeroed along with the rest
     * of the struct, so a handler that never defers is unaffected. */
    http_response_handle_t deferral;
    bool                   deferred;
} http_response_t;

/**
 * @brief Defer this response: return from the handler without answering,
 *        and complete it later via #http_server_respond.
 *
 * Call this from inside a request handler. The server will not serialise
 * anything when the handler returns; the connection is parked until the
 * handle is completed, the peer disconnects, or the deferral times out
 * (in which case the server answers 504 on the handler's behalf).
 *
 * This is what makes long-poll and other "answer when something happens"
 * patterns possible: without it a handler must block to wait, and because
 * the server is a single-threaded event loop, blocking one handler stalls
 * every other connection.
 *
 * @return #not_found if called outside a server request handler (nothing
 *         populated `deferral`), otherwise #ok.
 */
static inline result_t http_response_defer(http_response_t* response,
                                           http_response_handle_t* out_handle)
{
    if (response == NULL || out_handle == NULL)
    {
        return invalid_argument;
    }
    if (response->deferral.slot == NULL)
    {
        return not_found;
    }
    response->deferred = true;
    *out_handle        = response->deferral;
    return ok;
}

result_t http_response_initialize(http_response_t* response, span_t http_version, span_t code, span_t reason_phrase, http_headers_t headers);

static inline result_t http_response_set_body(http_response_t* response, span_t body)
{
    if (response == NULL) return invalid_argument;
    response->body = body;
    return ok;
}

static inline result_t http_response_get_body(http_response_t* response, span_t* body)
{
    if (response == NULL || body == NULL) return invalid_argument;
    *body = response->body;
    return ok;
}

static inline result_t http_response_get_code(http_response_t response, span_t* code)
{
    if (code == NULL)
    {
        return invalid_argument;
    }
    else
    {
        *code = response.code;
        return ok;
    }
}

static inline result_t http_response_get_reason_phrase(http_response_t response, span_t* reason_phrase)
{
    if (reason_phrase == NULL)
    {
        return invalid_argument;
    }
    else
    {
        *reason_phrase = response.reason_phrase;
        return ok;
    }
}

static inline result_t http_response_get_http_version(http_response_t response, span_t* http_version)
{
    if (http_version == NULL)
    {
        return invalid_argument;
    }
    else
    {
        *http_version = response.http_version;
        return ok;
    }
}

result_t http_response_parse(http_response_t* response, span_t raw_response, span_t* out_raw_response_remainder);

result_t http_response_serialize_to(http_response_t* response, stream_t* stream);

#endif // HTTP_RESPONSE_H
