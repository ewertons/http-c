#include "multipart.h"

#include <string.h>

#include "logging.h"

static const span_t CRLF      = span_from_str_literal("\r\n");
static const span_t CRLFCRLF  = span_from_str_literal("\r\n\r\n");
static const span_t DASHDASH  = span_from_str_literal("--");
static const span_t COLON     = span_from_str_literal(":");
static const span_t SEMICOLON = span_from_str_literal(";");
static const span_t EQUALS    = span_from_str_literal("=");

/* Find the value of a parameter `key=value` inside a header value like
 *   multipart/form-data; boundary=abcdef
 * Handles optional surrounding double quotes around the value. */
static span_t param_value(span_t header, span_t key)
{
    span_t segment, remainder = header;

    while (span_iterate(remainder, SEMICOLON, &segment, &remainder) == ok)
    {
        /* Trim leading spaces */
        while (!span_is_empty(segment) && span_get(segment, 0) == ' ')
        {
            segment = span_slice_to_end(segment, 1);
        }

        span_t seg_key, seg_val;
        if (span_split(segment, 0, EQUALS, &seg_key, &seg_val) != 0)
        {
            continue;
        }

        if (span_icompare(seg_key, key, true) != 0)
        {
            continue;
        }

        /* Strip surrounding double quotes if present */
        if (!span_is_empty(seg_val) && span_get(seg_val, 0) == '"')
        {
            seg_val = span_slice_to_end(seg_val, 1);
            if (!span_is_empty(seg_val) &&
                span_get(seg_val, span_get_size(seg_val) - 1) == '"')
            {
                seg_val = span_slice(seg_val, 0, span_get_size(seg_val) - 1);
            }
        }

        log_info("Found parameter value: %.*s",
                 (int)span_get_size(seg_val), (char*)span_get_ptr(seg_val));
        return seg_val;
    }

    return SPAN_EMPTY;
}

/* Find the boundary delimiter "--<boundary>" within cursor starting at offset.
 * Returns the position or -1 if not found. */
static int find_boundary(span_t cursor, uint32_t offset, span_t boundary)
{
    span_t search_area = span_slice_to_end(cursor, offset);

    /* Look for "--" first, then verify boundary follows */
    int pos = 0;
    span_t rem = search_area;

    while (!span_is_empty(rem))
    {
        span_t after_match;
        int found = span_find(rem, 0, DASHDASH, &after_match);
        if (found < 0)
        {
            return -1;
        }

        uint32_t abs_pos = (uint32_t)(span_get_ptr(rem) - span_get_ptr(cursor)) + (uint32_t)found;
        span_t candidate = span_slice_to_end(cursor, abs_pos + 2);

        if (span_get_size(candidate) >= span_get_size(boundary) &&
            memcmp(span_get_ptr(candidate), span_get_ptr(boundary),
                   span_get_size(boundary)) == 0)
        {
            return (int)abs_pos;
        }

        /* Advance past this "--" and keep searching */
        rem = after_match;
        (void)pos;
    }

    return -1;
}

/* Parse one header line "Name: value\r\n" via span_split.
 * Advances *cursor past the line. Returns false at the blank line (end of headers). */
static bool parse_header_line(span_t* cursor, span_t* out_name, span_t* out_value)
{
    if (span_is_empty(*cursor))
    {
        return false;
    }

    /* End-of-headers: starts with CRLF */
    if (span_get_size(*cursor) >= 2 &&
        span_get(*cursor, 0) == '\r' && span_get(*cursor, 1) == '\n')
    {
        *cursor = span_slice_to_end(*cursor, 2);
        return false;
    }

    /* Extract one line up to CRLF */
    span_t line;
    span_t rest;
    int crlf_pos = span_find(*cursor, 0, CRLF, &rest);
    if (crlf_pos < 0)
    {
        line = *cursor;
        *cursor = SPAN_EMPTY;
    }
    else
    {
        line = span_slice(*cursor, 0, (uint32_t)crlf_pos);
        *cursor = rest;
    }

    /* Split at first ':' */
    if (span_split(line, 0, COLON, out_name, out_value) != 0)
    {
        return false;
    }

    /* Trim leading whitespace from value */
    while (!span_is_empty(*out_value) &&
           (span_get(*out_value, 0) == ' ' || span_get(*out_value, 0) == '\t'))
    {
        *out_value = span_slice_to_end(*out_value, 1);
    }

    return true;
}

result_t mp_iterator_init(mp_iterator_t* it, span_t content_type_header, span_t body)
{
    if (it == NULL) return invalid_argument;

    it->done     = true;
    it->cursor   = SPAN_EMPTY;
    it->boundary = SPAN_EMPTY;

    span_t b = param_value(content_type_header, span_from_str_literal("boundary"));
    if (span_is_empty(b)) return not_found;

    it->boundary = b;
    it->cursor   = body;
    it->done     = false;
    return ok;
}

result_t mp_iterator_next(mp_iterator_t* it, mp_part_t* out)
{
    if (it == NULL || out == NULL) return invalid_argument;
    if (it->done) return end_of_data;

    span_t c = it->cursor;

    /* Find first boundary "--<boundary>" */
    int bpos = find_boundary(c, 0, it->boundary);
    if (bpos < 0)
    {
        it->done = true;
        return end_of_data;
    }

    uint32_t after = (uint32_t)bpos + 2 + span_get_size(it->boundary);

    /* If "--" follows the boundary, it's the final closing boundary. */
    span_t tail = span_slice_to_end(c, after);
    if (span_get_size(tail) >= 2 &&
        span_get(tail, 0) == '-' && span_get(tail, 1) == '-')
    {
        it->done = true;
        return end_of_data;
    }

    /* Skip CRLF after boundary line */
    if (span_get_size(tail) >= 2 &&
        span_get(tail, 0) == '\r' && span_get(tail, 1) == '\n')
    {
        after += 2;
    }

    /* Find end of headers (CRLFCRLF) */
    span_t after_headers;
    span_t header_area = span_slice_to_end(c, after);
    int hend = span_find(header_area, 0, CRLFCRLF, &after_headers);
    if (hend < 0)
    {
        it->done = true;
        return end_of_data;
    }

    /* Headers block includes up to and including the last CRLF of headers */
    span_t hdrs_block = span_slice(header_area, 0, (uint32_t)hend + 2);
    uint32_t body_start = after + (uint32_t)hend + 4;

    /* Find next boundary to determine where part data ends */
    int next_b = find_boundary(c, body_start, it->boundary);
    if (next_b < 0)
    {
        it->done = true;
        return end_of_data;
    }

    /* Trim trailing CRLF before next boundary (part of delimiter per RFC 2046) */
    uint32_t data_end = (uint32_t)next_b;
    if (data_end >= body_start + 2 &&
        span_get(c, data_end - 2) == '\r' && span_get(c, data_end - 1) == '\n')
    {
        data_end -= 2;
    }

    /* Populate output */
    out->name         = SPAN_EMPTY;
    out->filename     = SPAN_EMPTY;
    out->content_type = SPAN_EMPTY;
    out->data         = span_slice(c, body_start, data_end - body_start);

    /* Parse headers for Content-Disposition and Content-Type */
    span_t hcursor = hdrs_block;
    span_t hname, hval;
    while (parse_header_line(&hcursor, &hname, &hval))
    {
        if (span_icompare(hname, span_from_str_literal("Content-Disposition"), true) == 0)
        {
            out->name     = param_value(hval, span_from_str_literal("name"));
            out->filename = param_value(hval, span_from_str_literal("filename"));
        }
        else if (span_icompare(hname, span_from_str_literal("Content-Type"), true) == 0)
        {
            out->content_type = hval;
        }
    }

    /* Advance cursor past current part to the next boundary */
    it->cursor = span_slice_to_end(c, (uint32_t)next_b);
    return ok;
}
