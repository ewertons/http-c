#ifndef HTTP_QUERY_H
#define HTTP_QUERY_H

#include <stdbool.h>
#include <stdint.h>
#include <span.h>
#include <niceties.h>

/*
 * http_query — read access to URL query strings.
 *
 * Inputs are accepted in any of these shapes (a leading scheme/host is
 * not required; this module operates on the request-target portion):
 *
 *     /path/to/resource?key=value&other=thing
 *     ?key=value&other=thing
 *     key=value&other=thing
 *
 * All lookups are case-sensitive, allocation-free, and return slices that
 * alias into the caller's buffer. Decoding is opt-in via the *_decoded
 * variants.
 */

/* ---------------------------------------------------------------------- */
/* Single-shot lookup                                                     */
/* ---------------------------------------------------------------------- */

/**
 * Locate the value of the named parameter inside @p path's query string.
 *
 * On success @p *out_value is the still-percent-encoded value slice and
 * aliases into @p path. The slice may be empty (e.g. for "?foo=").
 *
 * @return  #ok            parameter found
 *          #not_found     parameter not present (or no '?' in @p path)
 *          #invalid_argument @p out_value is NULL
 */
result_t http_query_find(span_t path, span_t name, span_t* out_value);

/**
 * Same as #http_query_find but percent-decodes the value into @p dest.
 *
 * Decoding rules (application/x-www-form-urlencoded compatible):
 *   - "%XX" (two hex digits) -> the corresponding byte
 *   - "+"                    -> space (0x20)
 *   - any other byte         -> copied verbatim
 *
 * On success @p *out_decoded is the prefix of @p dest that was actually
 * written.
 *
 * @return  #ok                  parameter found and decoded
 *          #not_found           parameter not present
 *          #insufficient_size   @p dest too small to hold the decoded value
 *          #unexpected_char     malformed "%XX" escape in the value
 *          #invalid_argument    @p out_decoded is NULL
 */
result_t http_query_find_decoded(span_t path, span_t name,
                                 span_t dest, span_t* out_decoded);

/* ---------------------------------------------------------------------- */
/* Iteration                                                              */
/* ---------------------------------------------------------------------- */

/**
 * Iterator state. Treat as opaque; allocate on the stack.
 */
typedef struct http_query_iterator
{
    span_t   _query;   /* slice starting just after '?' (or whole input) */
    uint32_t _cursor;
} http_query_iterator_t;

/**
 * Position @p iterator at the first parameter of @p path's query string.
 * If @p path has no '?', the iterator is initialised over the whole input
 * (so callers can pass either "/p?a=1&b=2" or "a=1&b=2").
 */
result_t http_query_iterator_init(http_query_iterator_t* iterator, span_t path);

/**
 * Advance the iterator by one parameter.
 *
 * On success @p *out_name and @p *out_value are aliasing slices into the
 * original input. Either may be empty.
 *
 * @return  #ok           a pair was produced
 *          #end_of_data  no more pairs
 *          #invalid_argument any output pointer is NULL
 */
result_t http_query_iterator_next(http_query_iterator_t* iterator,
                                  span_t* out_name, span_t* out_value);

/* ---------------------------------------------------------------------- */
/* Predicates                                                             */
/* ---------------------------------------------------------------------- */

/**
 * Cheap optimisation hint: returns true iff calling a percent-decoder on
 * @p value would change any byte. Equivalent to "@p value contains '%'
 * or '+'". Use this to skip an unnecessary copy when the encoded and
 * decoded representations are the same byte sequence.
 *
 * Note: this does NOT validate that any "%XX" sequences are well-formed.
 * For that, decode the value (or add a dedicated validator if you only
 * need the predicate without producing the decoded output).
 */
bool http_query_value_needs_decoding(span_t value);

#endif /* HTTP_QUERY_H */
