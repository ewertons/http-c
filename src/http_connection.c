#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <time.h>

#include "span.h"
#include "niceties.h"
#include "logging.h"
#include "socket_stream.h"
#include "task.h"

#include "http_endpoint.h"
#include "http_connection.h"
#include "http_headers.h"
#include "http_methods.h"

/* CLOCK_MONOTONIC so a wall-clock adjustment cannot make a deadline fire
 * early or never. */
static uint64_t io_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* 0 means the caller asked to wait forever. */
static uint64_t io_deadline(const http_connection_t* connection)
{
    return connection->io_timeout_ms == 0
               ? 0
               : io_now_ms() + (uint64_t)connection->io_timeout_ms;
}

/* try_again means the stream produced nothing this time round.
 *
 * No sleep: the blocking path already spent the time in the socket's receive
 * slice (see apply_io_slice), and the non-blocking path's caller owns the
 * waiting. The yield only stops a wait-forever non-blocking socket spinning. */
static result_t await_progress(http_connection_t* connection, uint64_t deadline)
{
    if (deadline == 0)
    {
        (void)sched_yield();
        return ok;
    }

    if (io_now_ms() >= deadline)
    {
        log_error("http_connection: no progress for %u ms; abandoning the exchange",
                  connection->io_timeout_ms);
        return error;
    }

    return ok;
}

/* ------------------------------------------------------------------ */
/* Internal: ensure at least `need` bytes are available in the read    */
/* window starting at buf[*pos].  Reads from the stream if needed,    */
/* appending into buf and advancing *len.  Returns ok when satisfied.  */
/* ------------------------------------------------------------------ */
static result_t ensure_buffered(http_connection_t* connection,
                                uint8_t* buf, uint32_t buf_cap,
                                uint32_t* len, uint32_t pos,
                                uint32_t need)
{
    uint64_t deadline = io_deadline(connection);

    while (*len - pos < need)
    {
        if (*len >= buf_cap) return insufficient_size;
        span_t dst  = span_init(buf + *len, buf_cap - *len);
        span_t got, after;
        result_t r = stream_read(&connection->stream, dst, &got, &after);
        if (r == try_again)
        {
            result_t w = await_progress(connection, deadline);
            if (is_error(w)) return w;
            continue;
        }
        if (is_error(r) || r == end_of_data || r == end_of_file)
        {
            return (r == end_of_file || r == end_of_data) ? error : r;
        }
        *len += span_get_size(got);
        deadline = io_deadline(connection); /* bytes arrived: refresh the budget */
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Internal: read a chunked transfer-encoded body.                     */
/*                                                                     */
/* `remainder` points into the caller's buffer just after the parsed   */
/* headers; it may already contain the beginning of the chunk stream.  */
/* The dechunked body is written compactly into `out_buf` (which may   */
/* overlap with the remainder region — we always copy forward so this  */
/* is safe as long as out_buf <= remainder_ptr).                       */
/* ------------------------------------------------------------------ */
static result_t read_chunked_body(http_connection_t* connection,
                                  uint8_t* out_buf,
                                  uint32_t out_cap,
                                  uint32_t* out_len,
                                  span_t   remainder,
                                  uint8_t* raw_buf,
                                  uint32_t raw_cap)
{
    /* Copy remainder into the beginning of raw_buf if not already there. */
    uint32_t rlen = span_get_size(remainder);
    uint8_t* rptr = span_get_ptr(remainder);
    if (rlen > 0 && rptr != raw_buf)
    {
        if (rlen > raw_cap) rlen = raw_cap;
        memmove(raw_buf, rptr, rlen);
    }
    uint32_t raw_len = rlen;  /* bytes currently in raw_buf */
    uint32_t pos = 0;         /* parse cursor within raw_buf */
    uint32_t olen = 0;        /* dechunked bytes written */

    for (;;)
    {
        /* --- Parse chunk-size line: hex digits terminated by \r\n --- */
        /* Ensure we have at least one byte to start parsing. */
        result_t r = ensure_buffered(connection, raw_buf, raw_cap,
                                     &raw_len, pos, 1);
        if (is_error(r)) return r;

        uint32_t chunk_size = 0;
        /* Read hex digits. */
        while (pos < raw_len)
        {
            uint8_t c = raw_buf[pos];
            if      (c >= '0' && c <= '9') { chunk_size = chunk_size * 16 + (c - '0'); pos++; }
            else if (c >= 'a' && c <= 'f') { chunk_size = chunk_size * 16 + (c - 'a' + 10); pos++; }
            else if (c >= 'A' && c <= 'F') { chunk_size = chunk_size * 16 + (c - 'A' + 10); pos++; }
            else break; /* hit \r, \n, or ';' (chunk extension) */
        }
        /* Skip any chunk extensions (everything up to \r\n). */
        for (;;)
        {
            r = ensure_buffered(connection, raw_buf, raw_cap,
                                &raw_len, pos, 1);
            if (is_error(r)) return r;
            if (raw_buf[pos] == '\n') { pos++; break; }
            pos++;
        }

        if (chunk_size == 0) break; /* Last chunk. */

        /* --- Read chunk_size bytes of data --- */
        uint32_t remaining = chunk_size;
        while (remaining > 0)
        {
            /* Ensure we have at least 1 byte of chunk data. */
            r = ensure_buffered(connection, raw_buf, raw_cap,
                                &raw_len, pos, 1);
            if (is_error(r)) return r;

            uint32_t avail = raw_len - pos;
            uint32_t take = (avail < remaining) ? avail : remaining;
            if (olen + take > out_cap) take = out_cap - olen;
            if (take == 0) return insufficient_size;

            /* memmove, not memcpy: out_buf and raw_buf are allowed to be
             * the same buffer (that is how the caller avoids a scratch
             * allocation). olen <= pos always holds -- output trails
             * input because chunk-size lines are consumed and not
             * emitted -- so this is a forward copy, but the regions do
             * overlap and memcpy would be undefined. */
            memmove(out_buf + olen, raw_buf + pos, take);
            olen  += take;
            pos   += take;
            remaining -= take;
        }

        /* --- Skip trailing \r\n after chunk data --- */
        r = ensure_buffered(connection, raw_buf, raw_cap,
                            &raw_len, pos, 2);
        if (is_error(r)) return r;
        if (raw_buf[pos] == '\r') pos++;
        if (raw_buf[pos] == '\n') pos++;
    }

    /* Skip optional trailers + final \r\n after the last chunk. */
    /* (We don't parse trailers; just consume them.) */

    *out_len = olen;
    return ok;
}

result_t http_connection_set_endpoint(http_connection_t* connection, http_endpoint_t* endpoint)
{
    result_t result;

    if (connection == NULL || endpoint == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        connection->endpoint = endpoint;

        result = ok;
    }

    return result;
}

result_t http_connection_get_endpoint(http_connection_t* connection, http_endpoint_t** endpoint)
{
    result_t result;

    if (connection == NULL || endpoint == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        *endpoint = connection->endpoint;

        result = ok;
    }

    return result;
}

/* Read until the headers terminator (CRLFCRLF) is found, or the buffer is
 * full, or the stream returns an error. Retries on `try_again`. */
static result_t read_until_headers_complete(http_connection_t* connection,
                                            span_t buffer,
                                            span_t* out_received,
                                            span_t* out_remainder)
{
    span_t original_buffer = buffer;
    span_t bytes_read;
    result_t result;
    uint64_t deadline = io_deadline(connection);

    for (;;)
    {
        if (span_is_empty(buffer))
        {
            return insufficient_size;
        }

        result = stream_read(&connection->stream, buffer, &bytes_read, &buffer);

        if (result == try_again)
        {
            result_t w = await_progress(connection, deadline);
            if (is_error(w)) return w;
            continue;
        }

        if (is_error(result) || result == end_of_data || result == end_of_file)
        {
            return result;
        }

        deadline = io_deadline(connection);

        span_t received = span_slice_out(original_buffer, buffer);

        if (span_find_reverse(received, -1, headers_terminator) != -1)
        {
            *out_received  = received;
            *out_remainder = buffer;
            return ok;
        }
    }
}

/* Reads `needed` more bytes from the stream into the area immediately after
 * `body_so_far`. Updates `body_so_far` to span the full body. */
static result_t read_remaining_body(http_connection_t* connection,
                                    span_t* body_so_far,
                                    span_t free_buffer,
                                    uint32_t needed)
{
    uint64_t deadline = io_deadline(connection);

    while (span_get_size(*body_so_far) < needed)
    {
        if (span_is_empty(free_buffer))
        {
            return insufficient_size;
        }

        span_t got;
        result_t r = stream_read(&connection->stream, free_buffer, &got, &free_buffer);

        if (r == try_again)
        {
            result_t w = await_progress(connection, deadline);
            if (is_error(w)) return w;
            continue;
        }

        if (is_error(r) || r == end_of_data || r == end_of_file)
        {
            return r;
        }

        deadline = io_deadline(connection);

        /* If the body was empty so far, anchor it at the start of the bytes
         * just read; otherwise it's already pointing into the buffer and the
         * fresh bytes are contiguous with it. */
        if (span_is_empty(*body_so_far))
        {
            *body_so_far = got;
        }
        else
        {
            body_so_far->length += span_get_size(got);
        }
    }
    return ok;
}

/* Case-insensitive search for "chunked" anywhere in a Transfer-Encoding
 * value (could be "chunked" or "gzip, chunked" etc.). */
static bool transfer_encoding_is_chunked(span_t te_value)
{
    uint32_t te_len = span_get_size(te_value);
    const char* te_str = (const char*)span_get_ptr(te_value);
    if (te_len >= 7)
    {
        for (uint32_t i = 0; i + 7 <= te_len; i++)
        {
            if ((te_str[i]   == 'c' || te_str[i]   == 'C') &&
                (te_str[i+1] == 'h' || te_str[i+1] == 'H') &&
                (te_str[i+2] == 'u' || te_str[i+2] == 'U') &&
                (te_str[i+3] == 'n' || te_str[i+3] == 'N') &&
                (te_str[i+4] == 'k' || te_str[i+4] == 'K') &&
                (te_str[i+5] == 'e' || te_str[i+5] == 'E') &&
                (te_str[i+6] == 'd' || te_str[i+6] == 'D'))
            {
                return true;
            }
        }
    }
    return false;
}

static result_t maybe_read_body(http_connection_t* connection,
                                http_headers_t* headers,
                                span_t* body,
                                span_t free_buffer)
{
    static const span_t HDR_CONTENT_LENGTH = span_from_str_literal("Content-Length");
    static const span_t HDR_TRANSFER_ENCODING = span_from_str_literal("Transfer-Encoding");

    /* 1. Content-Length takes priority. */
    span_t value;
    if (http_headers_find(headers, HDR_CONTENT_LENGTH, &value) == HL_RESULT_OK)
    {
        uint32_t content_length = 0;
        if (span_to_uint32_t(value, &content_length) != 0)
        {
            return error;
        }
        if (content_length == 0) return ok;
        return read_remaining_body(connection, body, free_buffer, content_length);
    }

    /* 2. Check for Transfer-Encoding: chunked. */
    span_t te_value;
    if (http_headers_find(headers, HDR_TRANSFER_ENCODING, &te_value) == HL_RESULT_OK)
    {
        if (transfer_encoding_is_chunked(te_value))
        {
            /* Dechunk in place, inside the caller's own buffer.
             *
             * The raw chunk stream and the dechunked output share this
             * one region: the output always trails the parse cursor
             * (chunk-size lines are consumed but not emitted), so the
             * forward copy inside read_chunked_body never overwrites
             * bytes it has yet to read. This used to malloc a second
             * buffer of the same size for the raw stream, which cost an
             * allocation on every chunked response received and doubled
             * the peak memory; the library allocates nowhere else on
             * this path. */
            uint8_t* out_ptr = span_get_ptr(free_buffer);
            uint32_t out_cap = span_get_size(free_buffer);

            uint32_t body_len = 0;
            result_t r = read_chunked_body(connection,
                                           out_ptr, out_cap, &body_len,
                                           *body, /* remainder from header read */
                                           out_ptr, out_cap);

            if (is_error(r)) return r;

            *body = span_init(out_ptr, body_len);
            return ok;
        }
    }

    /* 3. No Content-Length, not chunked — no body. Only requests come
     * through here, and for a request that is the rule (RFC 9112 section
     * 6.3); responses are framed by read_response_body below. */
    return ok;
}

/* ------------------------------------------------------------------ */
/* Response bodies.                                                    */
/*                                                                     */
/* One reader serves both receive_response flavours. It reads into a   */
/* window of the caller's buffer and hands each piece of body on:      */
/*   - streaming: to the caller's handler, straight from the window,   */
/*     which is then reused, so the body may be of any size;           */
/*   - stored: copied down to the front of the window, where the body  */
/*     accumulates. The copy is always to a lower address (framing     */
/*     bytes are consumed, not stored), and the window is only ever    */
/*     compacted down to the end of the stored body, never over it.    */
/* ------------------------------------------------------------------ */

typedef enum response_body_framing
{
    response_body_framing_none,
    response_body_framing_content_length,
    response_body_framing_chunked,
    response_body_framing_until_close
} response_body_framing_t;

typedef struct response_body_reader
{
    http_connection_t* connection;
    uint8_t* buffer;
    uint32_t capacity;
    uint32_t start;          /* first received byte not yet consumed */
    uint32_t end;            /* end of the received bytes */
    uint32_t stored_length;  /* stored mode: body bytes kept at buffer[0..) */
    http_response_body_handler_t body_handler; /* NULL selects stored mode */
    void* body_handler_context;
} response_body_reader_t;

/* Receives more bytes at the end of the window. Returns end_of_data when the
 * peer has closed, insufficient_size when the window is full. */
static result_t response_body_reader_fill(response_body_reader_t* reader)
{
    uint32_t floor = reader->stored_length;

    if (reader->start == reader->end)
    {
        reader->start = floor;
        reader->end   = floor;
    }
    else if (reader->end == reader->capacity && reader->start > floor)
    {
        uint32_t pending = reader->end - reader->start;
        memmove(reader->buffer + floor, reader->buffer + reader->start, pending);
        reader->start = floor;
        reader->end   = floor + pending;
    }

    if (reader->end == reader->capacity)
    {
        return insufficient_size;
    }

    uint64_t deadline = io_deadline(reader->connection);

    for (;;)
    {
        span_t received = SPAN_EMPTY;
        result_t result = stream_read(&reader->connection->stream,
                                      span_init(reader->buffer + reader->end,
                                                reader->capacity - reader->end),
                                      &received, NULL);
        if (result == try_again)
        {
            result_t wait_result = await_progress(reader->connection, deadline);
            if (is_error(wait_result)) return wait_result;
            continue;
        }
        if (result == end_of_data || result == end_of_file)
        {
            return end_of_data;
        }
        if (is_error(result))
        {
            return result;
        }
        if (span_is_empty(received))
        {
            continue;
        }
        reader->end += span_get_size(received);
        return ok;
    }
}

static result_t response_body_reader_deliver(response_body_reader_t* reader, uint32_t length)
{
    uint8_t* piece = reader->buffer + reader->start;
    reader->start += length;

    if (reader->body_handler != NULL)
    {
        result_t result = reader->body_handler(reader->body_handler_context, span_init(piece, length));
        return is_error(result) ? result : ok;
    }

    memmove(reader->buffer + reader->stored_length, piece, length);
    reader->stored_length += length;
    return ok;
}

/* Passes exactly `length` body bytes on; the peer closing first is an error. */
static result_t response_body_reader_pass_through(response_body_reader_t* reader, uint32_t length)
{
    while (length > 0)
    {
        if (reader->start == reader->end)
        {
            result_t result = response_body_reader_fill(reader);
            if (result == end_of_data)
            {
                log_error("http_connection: connection closed with %u body bytes outstanding", length);
                return error;
            }
            if (is_error(result)) return result;
        }

        uint32_t available = reader->end - reader->start;
        uint32_t take = available < length ? available : length;
        result_t result = response_body_reader_deliver(reader, take);
        if (is_error(result)) return result;
        length -= take;
    }
    return ok;
}

/* Consumes one line and returns it without its CRLF (or bare LF). */
static result_t response_body_reader_read_line(response_body_reader_t* reader, span_t* out_line)
{
    uint32_t scanned = 0;

    for (;;)
    {
        for (uint32_t index = reader->start + scanned; index < reader->end; index++)
        {
            if (reader->buffer[index] == '\n')
            {
                uint32_t line_end = index;
                if (line_end > reader->start && reader->buffer[line_end - 1] == '\r')
                {
                    line_end--;
                }
                *out_line = span_init(reader->buffer + reader->start, line_end - reader->start);
                reader->start = index + 1;
                return ok;
            }
        }
        scanned = reader->end - reader->start;

        result_t result = response_body_reader_fill(reader);
        if (result == end_of_data)
        {
            log_error("http_connection: connection closed inside chunked framing");
            return error;
        }
        if (result == insufficient_size)
        {
            log_error("http_connection: chunked framing line does not fit the buffer");
        }
        if (is_error(result)) return result;
    }
}

static result_t response_body_reader_chunked(response_body_reader_t* reader)
{
    for (;;)
    {
        span_t line;
        result_t result = response_body_reader_read_line(reader, &line);
        if (is_error(result)) return result;

        uint32_t chunk_size = 0;
        uint32_t digits = 0;
        uint32_t index = 0;
        for (; index < span_get_size(line); index++)
        {
            uint8_t character = span_get(line, index);
            uint32_t value;
            if      (character >= '0' && character <= '9') value = (uint32_t)(character - '0');
            else if (character >= 'a' && character <= 'f') value = (uint32_t)(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F') value = (uint32_t)(character - 'A' + 10);
            else break;

            if (chunk_size > (UINT32_MAX >> 4))
            {
                log_error("http_connection: chunk size overflows");
                return error;
            }
            chunk_size = (chunk_size << 4) | value;
            digits++;
        }

        /* After the size only whitespace or a chunk extension may follow. */
        uint8_t next = index < span_get_size(line) ? span_get(line, index) : (uint8_t)';';
        if (digits == 0 || (next != ';' && next != ' ' && next != '\t'))
        {
            log_error("http_connection: malformed chunk size line");
            return error;
        }

        if (chunk_size == 0)
        {
            /* Trailer fields, discarded, up to the empty line. */
            do
            {
                result = response_body_reader_read_line(reader, &line);
                if (is_error(result)) return result;
            } while (!span_is_empty(line));
            return ok;
        }

        result = response_body_reader_pass_through(reader, chunk_size);
        if (is_error(result)) return result;

        result = response_body_reader_read_line(reader, &line);
        if (is_error(result)) return result;
        if (!span_is_empty(line))
        {
            log_error("http_connection: chunk data not followed by CRLF");
            return error;
        }
    }
}

static result_t response_body_reader_until_close(response_body_reader_t* reader)
{
    for (;;)
    {
        if (reader->start < reader->end)
        {
            result_t result = response_body_reader_deliver(reader, reader->end - reader->start);
            if (is_error(result)) return result;
        }

        result_t result = response_body_reader_fill(reader);
        if (result == end_of_data) return ok;
        if (is_error(result)) return result;
    }
}

/* RFC 9112 section 6.3, for a response. */
static result_t determine_response_body_framing(http_connection_t* connection,
                                                http_response_t* response,
                                                response_body_framing_t* out_framing,
                                                uint32_t* out_content_length)
{
    static const span_t HDR_CONTENT_LENGTH = span_from_str_literal("Content-Length");
    static const span_t HDR_TRANSFER_ENCODING = span_from_str_literal("Transfer-Encoding");

    uint32_t status = 0;
    if (span_get_size(response->code) != 3 || span_to_uint32_t(response->code, &status) != 0)
    {
        log_error("http_connection: malformed response status code");
        return error;
    }

    *out_content_length = 0;

    if ((status >= 100 && status <= 199) || status == 204 || status == 304 ||
        connection->awaiting_response_to_head)
    {
        *out_framing = response_body_framing_none;
        return ok;
    }

    /* Content-Length first, as for requests (maybe_read_body). */
    span_t value;
    if (http_headers_find(&response->headers, HDR_CONTENT_LENGTH, &value) == HL_RESULT_OK)
    {
        if (span_to_uint32_t(value, out_content_length) != 0)
        {
            log_error("http_connection: malformed Content-Length");
            return error;
        }
        *out_framing = *out_content_length == 0 ? response_body_framing_none
                                                : response_body_framing_content_length;
        return ok;
    }

    if (http_headers_find(&response->headers, HDR_TRANSFER_ENCODING, &value) == HL_RESULT_OK &&
        transfer_encoding_is_chunked(value))
    {
        *out_framing = response_body_framing_chunked;
        return ok;
    }

    *out_framing = response_body_framing_until_close;
    return ok;
}

/* Reads the status line and headers into buffer, then the body: stored
 * after the headers when body_handler is NULL, otherwise streamed to it. */
static result_t receive_response(http_connection_t* connection,
                                 span_t buffer,
                                 http_response_t* response,
                                 http_response_body_handler_t body_handler,
                                 void* body_handler_context,
                                 span_t* out_buffer_remainder)
{
    span_t received, remainder;
    result_t result = read_until_headers_complete(connection, buffer, &received, &remainder);

    if (is_error(result))
    {
        return result;
    }
    if (result == end_of_data || result == end_of_file)
    {
        log_error("http_connection: connection closed before the response headers were complete");
        return error;
    }

    result = http_response_parse(response, received, NULL);
    if (is_error(result))
    {
        return result;
    }

    /* Body bytes that arrived with the headers sit right before the unused
     * remainder of the buffer; together they are the body's window. */
    span_t prefetched = response->body;
    response->body = SPAN_EMPTY;

    response_body_reader_t reader;
    (void)memset(&reader, 0, sizeof(reader));
    reader.connection = connection;
    reader.buffer     = !span_is_empty(prefetched) ? span_get_ptr(prefetched) : span_get_ptr(remainder);
    reader.capacity   = span_get_size(prefetched) + span_get_size(remainder);
    reader.end        = span_get_size(prefetched);
    reader.body_handler         = body_handler;
    reader.body_handler_context = body_handler_context;

    response_body_framing_t framing;
    uint32_t content_length;
    result = determine_response_body_framing(connection, response, &framing, &content_length);
    if (is_error(result))
    {
        return result;
    }

    /* An interim (1xx) response is followed by the real one, which is still
     * the answer to the HEAD request. */
    if (span_get_size(response->code) == 3 && span_get(response->code, 0) != '1')
    {
        connection->awaiting_response_to_head = false;
    }

    switch (framing)
    {
        case response_body_framing_content_length:
            result = response_body_reader_pass_through(&reader, content_length);
            break;
        case response_body_framing_chunked:
            result = response_body_reader_chunked(&reader);
            break;
        case response_body_framing_until_close:
            result = response_body_reader_until_close(&reader);
            break;
        case response_body_framing_none:
        default:
            result = ok;
            break;
    }

    if (result == insufficient_size)
    {
        log_error("http_connection: response body does not fit the %u byte buffer area after the headers",
                  reader.capacity);
    }
    if (is_error(result))
    {
        return result;
    }

    if (body_handler == NULL && reader.stored_length > 0)
    {
        response->body = span_init(reader.buffer, reader.stored_length);
    }

    if (out_buffer_remainder != NULL)
    {
        *out_buffer_remainder = reader.capacity > reader.stored_length
                                    ? span_init(reader.buffer + reader.stored_length,
                                                reader.capacity - reader.stored_length)
                                    : SPAN_EMPTY;
    }

    return ok;
}

result_t http_connection_receive_request(http_connection_t* connection, span_t buffer, http_request_t* request, span_t* out_buffer_remainder)
{
    if (connection == NULL || request == NULL)
    {
        return invalid_argument;
    }

    span_t received, remainder;
    result_t result = read_until_headers_complete(connection, buffer, &received, &remainder);

    if (is_error(result))
    {
        return result;
    }

    result = http_request_parse(request, received);
    if (is_error(result))
    {
        return result;
    }

    /* Pull body bytes if Content-Length declares them. */
    result = maybe_read_body(connection, &request->headers, &request->body, remainder);
    if (is_error(result))
    {
        return result;
    }

    /* Advance remainder past whatever body bytes were consumed in the buffer. */
    if (out_buffer_remainder != NULL)
    {
        uint32_t body_len = span_get_size(request->body);
        if (body_len <= span_get_size(remainder))
        {
            *out_buffer_remainder = span_slice_to_end(remainder, body_len);
        }
        else
        {
            *out_buffer_remainder = SPAN_EMPTY;
        }
    }

    return ok;
}

result_t http_connection_send_response(http_connection_t* connection, http_response_t* response)
{
    result_t result;

    if (connection == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        result = http_response_serialize_to(response, &connection->stream);
    }

    return result;
}

result_t http_connection_send_request(http_connection_t* connection, http_request_t* request)
{
    result_t result;

    if (connection == NULL || request == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        result = http_request_serialize_to(request, &connection->stream);
        connection->awaiting_response_to_head =
            span_compare(request->method, HTTP_METHOD_HEAD) == 0;
    }

    return result;
}

result_t http_connection_receive_response(http_connection_t* connection, span_t buffer, http_response_t* response, span_t* out_buffer_remainder)
{
    if (connection == NULL || response == NULL)
    {
        return invalid_argument;
    }

    return receive_response(connection, buffer, response, NULL, NULL, out_buffer_remainder);
}

result_t http_connection_receive_response_streaming(http_connection_t* connection, span_t header_buffer, http_response_t* response, http_response_body_handler_t body_handler, void* body_handler_context)
{
    if (connection == NULL || response == NULL || body_handler == NULL)
    {
        return invalid_argument;
    }

    return receive_response(connection, header_buffer, response, body_handler, body_handler_context, NULL);
}

result_t http_connection_close(http_connection_t* connection)
{
    result_t result;

    if (connection == NULL)
    {
        result = invalid_argument;
    }
    else
    {
        result = stream_close(&connection->stream);
        connection->endpoint = NULL;
    }

    return result;
}
