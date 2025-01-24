#ifndef http_headers_t_H
#define http_headers_t_H

#include <stdlib.h>
#include <span.h>
#include "common.h"

// https://tools.ietf.org/html/rfc2616#page-100

static const span_t HTTP_HEADER_AUTHORIZATION = span_from_str_literal("Authorization");


// General Headers

static const span_t HTTP_HEADER_CACHE_CONTROL = span_from_str_literal("Cache-Control");
static const span_t HTTP_HEADER_CONNECTION = span_from_str_literal("Connection");
static const span_t HTTP_HEADER_DATE = span_from_str_literal("Date");
static const span_t HTTP_HEADER_PRAGMA = span_from_str_literal("Pragma");
static const span_t HTTP_HEADER_TRAILER = span_from_str_literal("Trailer");
static const span_t HTTP_HEADER_TRANSFER_ENCODING = span_from_str_literal("Transfer-Encoding");
static const span_t HTTP_HEADER_UPGRADE = span_from_str_literal("Upgrade");
static const span_t HTTP_HEADER_VIA = span_from_str_literal("Via");
static const span_t HTTP_HEADER_WARNING = span_from_str_literal("Warning");


// Entity Headers

static const span_t HTTP_HEADER_ALLOW = span_from_str_literal("Allow");
static const span_t HTTP_HEADER_CONTENT_ENCODING = span_from_str_literal("Content-Encoding");
static const span_t HTTP_HEADER_CONTENT_LANGUAGE = span_from_str_literal("Content-Language");
static const span_t HTTP_HEADER_CONTENT_LENGTH = span_from_str_literal("Content-Length");
static const span_t HTTP_HEADER_CONTENT_LOCATION = span_from_str_literal("Content-Location");
static const span_t HTTP_HEADER_CONTENT_TYPE = span_from_str_literal("Content-Type");
static const span_t HTTP_HEADER_EXPIRES = span_from_str_literal("Expires");
static const span_t HTTP_HEADER_LAST_MODIFIED = span_from_str_literal("Last-Modified");


// Request Headers

static const span_t HTTP_HEADER_ACCEPT = span_from_str_literal("Accept");
static const span_t HTTP_HEADER_ACCEPT_CHARSET = span_from_str_literal("Accept-Charset");
static const span_t HTTP_HEADER_ACCEPT_ENCODING = span_from_str_literal("Accept-Encoding");
static const span_t HTTP_HEADER_ACCEPT_LANGUAGE = span_from_str_literal("Accept-Language");
static const span_t HTTP_HEADER_EXPECT = span_from_str_literal("Expect");
static const span_t HTTP_HEADER_FROM = span_from_str_literal("From");
static const span_t HTTP_HEADER_HOST = span_from_str_literal("Host");
static const span_t HTTP_HEADER_IF_MATCH = span_from_str_literal("If-Match");
static const span_t HTTP_HEADER_IF_MODIFIED_SINCE = span_from_str_literal("If-Modified-Since");
static const span_t HTTP_HEADER_IF_NONE_MATCH = span_from_str_literal("If-None-Match");
static const span_t HTTP_HEADER_IF_RANGE = span_from_str_literal("If-Range");
static const span_t HTTP_HEADER_IF_UNMODIFIED_SINCE = span_from_str_literal("If-Unmodified-Since");
static const span_t HTTP_HEADER_MAX_FORWARDS = span_from_str_literal("Max-Forwards");
static const span_t HTTP_HEADER_PROXY_AUTHORIZATION = span_from_str_literal("Proxy-Authorization");
static const span_t HTTP_HEADER_TE = span_from_str_literal("TE");
static const span_t HTTP_HEADER_USER_AGENT = span_from_str_literal("User-Agent");


// Response Headers

static const span_t HTTP_HEADER_ACCEPT_RANGES = span_from_str_literal("Accept-Ranges");
static const span_t HTTP_HEADER_AGE = span_from_str_literal("Age");
static const span_t HTTP_HEADER_ETAG = span_from_str_literal("Etag");
static const span_t HTTP_HEADER_LOCATION = span_from_str_literal("Location");
static const span_t HTTP_HEADER_PROXY_AUTHENTICATE = span_from_str_literal("Proxy-Authenticate");
static const span_t HTTP_HEADER_RETRY_AFTER = span_from_str_literal("Retry-After");
static const span_t HTTP_HEADER_SERVER = span_from_str_literal("Server");
static const span_t HTTP_HEADER_WWW_AUTHENTICATE = span_from_str_literal("WWW-Authenticate");


typedef struct http_headers
{
    span_t buffer;
    uint32_t used_size;
    span_t iterator;
} http_headers_t;

HL_RESULT http_headers_init(http_headers_t* headers, span_t buffer);

result_t http_headers_parse(http_headers_t* headers, span_t raw_headers);

HL_RESULT http_headers_find(http_headers_t* headers, span_t name, span_t* value);

HL_RESULT http_headers_get_next(http_headers_t* headers, span_t* name, span_t* value);

HL_RESULT http_headers_get_buffer(http_headers_t* headers, span_t* buffer);

HL_RESULT http_headers_add(http_headers_t* headers, span_t name, span_t value);


#endif // http_headers_t_H
