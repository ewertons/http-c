#ifndef HTTP_METHODS_H
#define HTTP_METHODS_H

#include <span.h>

// https://tools.ietf.org/html/rfc2616#page-36

static const span_t HTTP_METHOD_OPTIONS = span_from_str_literal("OPTIONS");
static const span_t HTTP_METHOD_GET = span_from_str_literal("GET");
static const span_t HTTP_METHOD_HEAD = span_from_str_literal("HEAD");
static const span_t HTTP_METHOD_POST = span_from_str_literal("POST");
static const span_t HTTP_METHOD_PUT = span_from_str_literal("PUT");
static const span_t HTTP_METHOD_DELETE = span_from_str_literal("DELETE");
static const span_t HTTP_METHOD_TRACE = span_from_str_literal("TRACE");
static const span_t HTTP_METHOD_CONNECT = span_from_str_literal("CONNECT");

#endif // HTTP_METHODS_H
