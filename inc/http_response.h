#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include "http_headers.h"
#include "http_request.h"
#include <span.h>

// https://tools.ietf.org/html/rfc2616#page-100


typedef struct http_response 
{
    http_request_t* request;
    span_t code;
    span_t reason_phrase;
    http_headers_t* headers;
    span_t body;
} http_response_t;


HL_RESULT http_response_init(http_response_t* response, http_request_t* request, span_t code, span_t reason_phrase);

span_t http_response_get_code(http_response_t response);

span_t http_response_get_reason_phrase(http_response_t response);

span_t http_response_get_http_version(http_response_t response);

HL_RESULT http_response_set_headers(http_response_t* response, http_headers_t* headers);

HL_RESULT http_response_get_headers(http_response_t response, http_headers_t** headers);

HL_RESULT http_response_set_body(http_response_t* response, span_t body);

void http_response_deinit(http_response_t* response);

#endif // HTTP_RESPONSE_H
