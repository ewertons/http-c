#ifndef HTTP_CODES_H
#define HTTP_CODES_H

#include <span.h>

static const span_t HTTP_CODE_200 = span_from_str_literal("200");
static const span_t HTTP_CODE_201 = span_from_str_literal("201");
static const span_t HTTP_CODE_202 = span_from_str_literal("202");
static const span_t HTTP_CODE_302 = span_from_str_literal("302");
static const span_t HTTP_CODE_400 = span_from_str_literal("400");
static const span_t HTTP_CODE_401 = span_from_str_literal("401");
static const span_t HTTP_CODE_404 = span_from_str_literal("404");
static const span_t HTTP_CODE_405 = span_from_str_literal("405");
static const span_t HTTP_CODE_406 = span_from_str_literal("406");
static const span_t HTTP_CODE_408 = span_from_str_literal("408");
static const span_t HTTP_CODE_409 = span_from_str_literal("409");
static const span_t HTTP_CODE_500 = span_from_str_literal("500");
static const span_t HTTP_CODE_501 = span_from_str_literal("501");
static const span_t HTTP_CODE_503 = span_from_str_literal("503");
static const span_t HTTP_CODE_505 = span_from_str_literal("505");

static const span_t HTTP_REASON_PHRASE_200 = span_from_str_literal("OK");
static const span_t HTTP_REASON_PHRASE_201 = span_from_str_literal("Created");
static const span_t HTTP_REASON_PHRASE_202 = span_from_str_literal("Accepted");
static const span_t HTTP_REASON_PHRASE_302 = span_from_str_literal("Found");
static const span_t HTTP_REASON_PHRASE_400 = span_from_str_literal("Bad Request");
static const span_t HTTP_REASON_PHRASE_401 = span_from_str_literal("Unauthorized");
static const span_t HTTP_REASON_PHRASE_404 = span_from_str_literal("Not Found");
static const span_t HTTP_REASON_PHRASE_405 = span_from_str_literal("Method Not Allowed");
static const span_t HTTP_REASON_PHRASE_406 = span_from_str_literal("Not Acceptable");
static const span_t HTTP_REASON_PHRASE_408 = span_from_str_literal("Request Timeout");
static const span_t HTTP_REASON_PHRASE_409 = span_from_str_literal("Conflict");
static const span_t HTTP_REASON_PHRASE_500 = span_from_str_literal("Internal Server Error");
static const span_t HTTP_REASON_PHRASE_501 = span_from_str_literal("Not Implemented");
static const span_t HTTP_REASON_PHRASE_503 = span_from_str_literal("Service Unavailable");
static const span_t HTTP_REASON_PHRASE_505 = span_from_str_literal("HTTP Version Not Supported");

#endif // HTTP_CODES_H