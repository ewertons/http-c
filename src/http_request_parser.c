#include <string.h>

#include <span.h>
#include "niceties.h"

#include "common.h"
#include "http_headers.h"
#include "http_request.h"
#include "http_request_parser.h"

void http_request_parser_init(http_request_parser_t* parser)
{
    if (parser == NULL)
    {
        return;
    }
    (void)memset(parser, 0, sizeof(*parser));
    parser->state = http_request_parser_state_request_line;
}

/* Locate the CRLFCRLF that ends the header block. Returns the offset
 * (relative to `buffer`) of the byte AFTER the terminator, or -1 if not
 * yet present. */
static int find_headers_end(span_t buffer)
{
    span_t after = SPAN_EMPTY;
    int term = span_find(buffer, 0, headers_terminator, &after);
    if (term == -1)
    {
        return -1;
    }
    return term + (int)span_get_size(headers_terminator);
}

/* Parse the request line + headers from `buffer` whose length is at
 * least `headers_end`. Populates parser->request and reads
 * Content-Length. */
static result_t parse_head(http_request_parser_t* parser, span_t buffer,
                           uint32_t headers_end)
{
    /* Slice covering only the head (request line + headers + CRLFCRLF). */
    span_t head = span_slice(buffer, 0, headers_end);

    span_t method, path, version, rest;

    if (span_split(head, 0, space, &method, &rest) != 0) return error;
    if (span_split(rest, 0, space, &path, &rest) != 0)   return error;
    if (span_split(rest, 0, crlf,  &version, &rest) != 0) return error;

    parser->request.method       = method;
    parser->request.path         = path;
    parser->request.http_version = version;
    parser->request.body         = SPAN_EMPTY;

    if (failed(http_headers_parse(&parser->request.headers, rest)))
    {
        return error;
    }

    /* Look up Content-Length to know how many body bytes to wait for. */
    span_t cl_value;
    if (http_headers_find(&parser->request.headers,
                          HTTP_HEADER_CONTENT_LENGTH, &cl_value) == HL_RESULT_OK)
    {
        uint32_t cl = 0;
        if (span_to_uint32_t(cl_value, &cl) != 0)
        {
            return error;
        }
        parser->content_length     = cl;
        parser->has_content_length = true;
    }

    parser->headers_end = headers_end;
    return ok;
}

result_t http_request_parser_feed(http_request_parser_t* parser, span_t buffer)
{
    if (parser == NULL)
    {
        return invalid_argument;
    }

    switch (parser->state)
    {
        case http_request_parser_state_complete:
            return ok;
        case http_request_parser_state_error:
            return error;
        default:
            break;
    }

    /* Phase 1: locate end of header block if not done yet. */
    if (parser->headers_end == 0)
    {
        int end = find_headers_end(buffer);
        if (end == -1)
        {
            parser->state = (span_get_size(buffer) == 0)
                              ? http_request_parser_state_request_line
                              : http_request_parser_state_headers;
            return try_again;
        }

        result_t pr = parse_head(parser, buffer, (uint32_t)end);
        if (failed(pr))
        {
            parser->state = http_request_parser_state_error;
            return error;
        }
        parser->state = http_request_parser_state_body;
    }

    /* Phase 2: wait for body bytes. */
    uint32_t needed = parser->headers_end + parser->content_length;
    if (span_get_size(buffer) < needed)
    {
        parser->state = http_request_parser_state_body;
        return try_again;
    }

    if (parser->content_length > 0)
    {
        parser->request.body =
            span_slice(buffer, parser->headers_end, parser->content_length);
    }
    else
    {
        parser->request.body = SPAN_EMPTY;
    }

    parser->state = http_request_parser_state_complete;
    return ok;
}
