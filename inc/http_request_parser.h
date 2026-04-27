#ifndef HTTP_REQUEST_PARSER_H
#define HTTP_REQUEST_PARSER_H

#include <stdbool.h>
#include <stdint.h>

#include <span.h>
#include "niceties.h"

#include "http_request.h"

/* ------------------------------------------------------------------------- *
 * Incremental, zero-copy HTTP request parser.
 *
 * Design contract:
 *  - The caller owns a single contiguous buffer that grows as bytes arrive
 *    from the transport. After each read, the caller invokes
 *    `http_request_parser_feed` with a span covering the FULL accumulated
 *    buffer (offset 0 up to the number of bytes received so far).
 *  - The parser is stateless w.r.t. data: it scans the buffer from the
 *    beginning each call but cheaply short-circuits once headers are
 *    parsed by remembering `headers_end` and `content_length`.
 *  - All spans inside the produced `http_request_t` point into the
 *    caller's buffer. The buffer must outlive request usage.
 *  - No dynamic allocation. No I/O. Safe to call from an event-loop
 *    handler.
 * ------------------------------------------------------------------------- */

typedef enum http_request_parser_state
{
    http_request_parser_state_request_line = 0,
    http_request_parser_state_headers,
    http_request_parser_state_body,
    http_request_parser_state_complete,
    http_request_parser_state_error,
} http_request_parser_state_t;

typedef struct http_request_parser
{
    http_request_parser_state_t state;
    http_request_t              request;

    /* Offset (from start of buffer) past the CRLFCRLF that ends the
     * header block. 0 until headers complete. */
    uint32_t headers_end;

    /* Body length declared by Content-Length, if any. */
    uint32_t content_length;
    bool     has_content_length;
} http_request_parser_t;

/**
 * @brief Reset a parser to the request-line state.
 */
void http_request_parser_init(http_request_parser_t* parser);

/**
 * @brief Feed the parser the entire accumulated buffer.
 *
 * @param parser  Parser instance.
 * @param buffer  Span covering all bytes received so far on this connection.
 *
 * @return ok        Request fully parsed; call `http_request_parser_get_request`.
 *         try_again More bytes are required.
 *         error     Malformed input — connection should be closed.
 *         invalid_argument  NULL parser.
 */
result_t http_request_parser_feed(http_request_parser_t* parser, span_t buffer);

static inline http_request_parser_state_t
http_request_parser_get_state(const http_request_parser_t* parser)
{
    return (parser == NULL) ? http_request_parser_state_error : parser->state;
}

static inline http_request_t*
http_request_parser_get_request(http_request_parser_t* parser)
{
    return (parser == NULL) ? NULL : &parser->request;
}

/**
 * @brief Total number of bytes that constitute the parsed request
 *        (request line + headers + body). 0 until state == complete.
 *        Useful for keep-alive: caller can shift any trailing pipelined
 *        bytes down to offset 0 and re-init the parser.
 */
static inline uint32_t
http_request_parser_get_consumed(const http_request_parser_t* parser)
{
    if (parser == NULL || parser->state != http_request_parser_state_complete)
    {
        return 0;
    }
    return parser->headers_end + parser->content_length;
}

#endif /* HTTP_REQUEST_PARSER_H */
