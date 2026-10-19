#ifndef HTTP_CONNECTION_H
#define HTTP_CONNECTION_H

#include <span.h>
#include "niceties.h"
#include "stream.h"
#include "socketx.h"

#include "http_endpoint.h"
#include "http_request.h"
#include "http_response.h"



typedef struct http_connection_config
{

} http_connection_config_t;

typedef struct http_connection
{
    http_endpoint_t* endpoint;
    socket_t socket;
    stream_t stream;

    /* Copied from the endpoint at connect/accept. See io_timeout_ms there. */
    uint32_t io_timeout_ms;

    /* Set by http_connection_send_request when the request method is HEAD,
     * so the response to it is read without a body (RFC 9112 section 6.3)
     * whatever its Content-Length or Transfer-Encoding say. */
    bool awaiting_response_to_head;
} http_connection_t;

/* Receives one piece of a response body, already de-chunked.
 *
 * body_chunk points into the scratch area of the header_buffer given to
 * http_connection_receive_response_streaming and is only valid during the
 * call; copy what must be kept. Never called with an empty chunk.
 *
 * Returning a failed result (is_error) stops the transfer;
 * http_connection_receive_response_streaming then returns that result and
 * the rest of the body is left unread, so the connection should be closed. */
typedef result_t (*http_response_body_handler_t)(void* context, span_t body_chunk);

result_t http_connection_set_endpoint(http_connection_t* connection, http_endpoint_t* endpoint);

result_t http_connection_get_endpoint(http_connection_t* connection, http_endpoint_t** endpoint);

result_t http_connection_receive_request(http_connection_t* connection, span_t buffer, http_request_t* request, span_t* out_buffer_remainder);

result_t http_connection_send_response(http_connection_t* connection, http_response_t* response);

result_t http_connection_send_request(http_connection_t* connection, http_request_t* request);

/* Receives a response, headers and body, into buffer.
 *
 * The body is framed as RFC 9112 section 6.3 describes:
 *   - none for 1xx, 204 and 304 responses, and for the response to a HEAD
 *     request sent with http_connection_send_request on this connection;
 *   - Content-Length bytes when that header is present;
 *   - de-chunked in place when Transfer-Encoding is chunked (trailers are
 *     read and discarded);
 *   - otherwise everything up to the peer closing the connection.
 *
 * response->body spans the body inside buffer. Returns insufficient_size
 * when headers and body together do not fit; use
 * http_connection_receive_response_streaming for bodies of unbounded size.
 *
 * out_buffer_remainder (optional) receives the part of buffer after the
 * body. */
result_t http_connection_receive_response(http_connection_t* connection, span_t buffer, http_response_t* response, span_t* out_buffer_remainder);

/* Receives a response whose body is handed to body_handler piece by piece
 * instead of being stored, so the body may be of any size.
 *
 * The status line and headers are parsed into response exactly as by
 * http_connection_receive_response, using the start of header_buffer; their
 * spans stay valid until header_buffer is reused. Whatever header_buffer has
 * left after the headers is the scratch area that body pieces are read into,
 * so a piece is never larger than that remainder. Size header_buffer for the
 * largest expected header block plus a scratch area of a few KiB or more
 * (16 KiB is a reasonable choice); a chunk-size line or trailer that does
 * not fit the scratch area fails with insufficient_size.
 *
 * Framing follows the same rules as http_connection_receive_response
 * (none / Content-Length / chunked / until close). response->body is left
 * empty. A response without a body still succeeds, with body_handler never
 * called.
 *
 * Returns ok once the whole body has been delivered, the first failed result
 * of body_handler, error if the peer closes before a Content-Length or
 * chunked body is complete or stays silent past io_timeout_ms, or
 * insufficient_size as described above. */
result_t http_connection_receive_response_streaming(http_connection_t* connection, span_t header_buffer, http_response_t* response, http_response_body_handler_t body_handler, void* body_handler_context);

result_t http_connection_close(http_connection_t* connection);

#endif // HTTP_CONNECTION_H
