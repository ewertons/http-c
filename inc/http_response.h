#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

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
} http_response_t;

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
