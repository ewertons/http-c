#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include "http_headers.h"

#include "span.h" 
#include "niceties.h" 

// https://tools.ietf.org/html/rfc2616#page-100


typedef struct http_response 
{
    span_t http_version;
    span_t code;
    span_t reason_phrase;
    http_headers_t headers;
} http_response_t;

result_t http_response_initialize(http_response_t* response, span_t http_version, span_t code, span_t reason_phrase, http_headers_t headers);

// TODO: Change all these to be accessors only... remove code.

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
