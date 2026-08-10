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
 * There is no sleep here: on the blocking path the socket's own receive slice
 * (see apply_io_slice) has already spent that time, and on the non-blocking
 * path the caller owns the waiting. Yielding covers the wait-forever case so
 * a non-blocking socket cannot become a hot spin. */
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
        /* Case-insensitive check for "chunked". */
        uint32_t te_len = span_get_size(te_value);
        const char* te_str = (const char*)span_get_ptr(te_value);
        bool is_chunked = false;
        if (te_len >= 7)
        {
            /* Look for "chunked" anywhere in the value (could be
             * "chunked" or "gzip, chunked" etc.). */
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
                    is_chunked = true;
                    break;
                }
            }
        }

        if (is_chunked)
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

    /* 3. No Content-Length, not chunked — no body. */
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
    }

    return result;
}

result_t http_connection_receive_response(http_connection_t* connection, span_t buffer, http_response_t* response, span_t* out_buffer_remainder)
{
    if (connection == NULL || response == NULL)
    {
        return invalid_argument;
    }

    span_t received, remainder;
    result_t result = read_until_headers_complete(connection, buffer, &received, &remainder);

    if (is_error(result))
    {
        return result;
    }

    result = http_response_parse(response, received, NULL);
    if (is_error(result))
    {
        return result;
    }

    result = maybe_read_body(connection, &response->headers, &response->body, remainder);
    if (is_error(result))
    {
        return result;
    }

    if (out_buffer_remainder != NULL)
    {
        uint32_t body_len = span_get_size(response->body);
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
