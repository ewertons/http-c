#include "http_query.h"

#include <string.h>

/* Convert one ASCII hex digit to its 0..15 value, or -1 on failure. */
static int hex_value(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Return a span over the query-string portion of `path`: the substring
 * starting just after the first '?'. If there is no '?', return the
 * whole input — that lets callers pass either "/p?a=1" or "a=1". */
static span_t locate_query(span_t path)
{
    const uint8_t* p = span_get_ptr(path);
    uint32_t       n = span_get_size(path);
    for (uint32_t i = 0; i < n; i++)
    {
        if (p[i] == '?')
        {
            return span_slice(path, i + 1, n - (i + 1));
        }
    }
    return path;
}

/* Does `path` contain a '?'? Used to disambiguate "no query" from
 * "empty query". */
static bool path_has_question_mark(span_t path)
{
    const uint8_t* p = span_get_ptr(path);
    uint32_t       n = span_get_size(path);
    for (uint32_t i = 0; i < n; i++)
    {
        if (p[i] == '?') return true;
    }
    return false;
}

/* ---------------------------------------------------------------------- */
/* Iterator                                                               */
/* ---------------------------------------------------------------------- */

result_t http_query_iterator_init(http_query_iterator_t* iterator, span_t path)
{
    if (iterator == NULL)
    {
        return invalid_argument;
    }
    iterator->_query  = locate_query(path);
    iterator->_cursor = 0;
    return ok;
}

result_t http_query_iterator_next(http_query_iterator_t* iterator,
                                  span_t* out_name, span_t* out_value)
{
    if (iterator == NULL || out_name == NULL || out_value == NULL)
    {
        return invalid_argument;
    }

    const uint8_t* p = span_get_ptr(iterator->_query);
    uint32_t       n = span_get_size(iterator->_query);
    uint32_t       i = iterator->_cursor;

    /* Skip any leading '&' separators or stray empty pairs. */
    while (i < n && p[i] == '&') { i++; }
    if (i >= n)
    {
        iterator->_cursor = n;
        return end_of_data;
    }

    uint32_t name_start = i;
    while (i < n && p[i] != '=' && p[i] != '&') { i++; }
    uint32_t name_end = i;

    uint32_t value_start = name_end;
    uint32_t value_end   = name_end;
    if (i < n && p[i] == '=')
    {
        value_start = i + 1;
        i = value_start;
        while (i < n && p[i] != '&') { i++; }
        value_end = i;
    }
    /* If there was no '=' the value is an empty span at name_end. */

    *out_name  = span_slice(iterator->_query, name_start, name_end - name_start);
    *out_value = span_slice(iterator->_query, value_start, value_end - value_start);

    iterator->_cursor = (i < n) ? i + 1 : n;
    return ok;
}

/* ---------------------------------------------------------------------- */
/* Single-shot lookup                                                     */
/* ---------------------------------------------------------------------- */

result_t http_query_find(span_t path, span_t name, span_t* out_value)
{
    if (out_value == NULL)
    {
        return invalid_argument;
    }
    /* Distinguish "no '?' in path" (caller provided just a path) vs
     * "got a query string that legitimately omits the named field".
     * In the former case we still scan: callers may have passed the
     * raw query string already. */
    (void)path_has_question_mark;

    http_query_iterator_t it;
    (void)http_query_iterator_init(&it, path);

    span_t k, v;
    while (http_query_iterator_next(&it, &k, &v) == ok)
    {
        if (span_compare(k, name) == 0)
        {
            *out_value = v;
            return ok;
        }
    }
    return not_found;
}

result_t http_query_find_decoded(span_t path, span_t name,
                                 span_t dest, span_t* out_decoded)
{
    if (out_decoded == NULL)
    {
        return invalid_argument;
    }

    span_t raw;
    result_t r = http_query_find(path, name, &raw);
    if (failed(r))
    {
        return r;
    }

    const uint8_t* src = span_get_ptr(raw);
    uint32_t       sn  = span_get_size(raw);
    uint8_t*       dst = span_get_ptr(dest);
    uint32_t       dn  = span_get_size(dest);
    uint32_t       written = 0;

    for (uint32_t i = 0; i < sn; i++)
    {
        if (written >= dn)
        {
            return insufficient_size;
        }
        uint8_t c = src[i];
        if (c == '+')
        {
            dst[written++] = ' ';
        }
        else if (c == '%')
        {
            if (i + 2 >= sn) { return unexpected_char; }
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if (hi < 0 || lo < 0) { return unexpected_char; }
            dst[written++] = (uint8_t)((hi << 4) | lo);
            i += 2;
        }
        else
        {
            dst[written++] = c;
        }
    }
    *out_decoded = span_slice(dest, 0, written);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* Predicate                                                              */
/* ---------------------------------------------------------------------- */

bool http_query_value_needs_decoding(span_t value)
{
    const uint8_t* p = span_get_ptr(value);
    uint32_t       n = span_get_size(value);
    for (uint32_t i = 0; i < n; i++)
    {
        if (p[i] == '%' || p[i] == '+')
        {
            return true;
        }
    }
    return false;
}
