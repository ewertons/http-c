#ifndef HTTP_VERSIONS_H
#define HTTP_VERSIONS_H

#include <span.h>

static const span_t HTTP_VERSION_1_1 = span_from_str_literal("HTTP/1.1");
static const span_t HTTP_VERSION_2_0 = span_from_str_literal("HTTP/2.0");

#endif // HTTP_VERSIONS_H