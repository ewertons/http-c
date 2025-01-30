#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include <inttypes.h>

#include "niceties.h"

#include "http_response.h"
#include "http_versions.h"
#include "http_codes.h"

#include <test_http.h>

static uint8_t TEST_HTTP_RESPONSE_200_OK_1[] = "HTTP/1.1 200 OK\r\n\
Date: Mon, 27 Jul 2009 12:28:53 GMT\r\n\
Server: Apache/2.2.14 (Win32)\r\n\
Last-Modified: Wed, 22 Jul 2009 19:15:56 GMT\r\n\
Content-Length: 88\r\n\
Content-Type: text/html\r\n\
Connection: Closed\r\n";

static uint8_t raw_buffer[1024];

static void http_response_init_succeed(void** state)
{
    (void)state;

    http_response_t response;
    span_t version = HTTP_VERSION_1_1;
    span_t code = HTTP_CODE_200;
    span_t reason_phrase = HTTP_REASON_PHRASE_200;
    http_headers_t headers = { 0 };

    assert_int_equal(http_response_initialize(&response, version, code, reason_phrase, headers), ok);
    assert_memory_equal(span_get_ptr(response.http_version), span_get_ptr(HTTP_VERSION_1_1), span_get_size(response.http_version));
    assert_memory_equal(span_get_ptr(response.code), span_get_ptr(HTTP_CODE_200), span_get_size(response.code));
    assert_memory_equal(span_get_ptr(response.reason_phrase), span_get_ptr(HTTP_REASON_PHRASE_200), span_get_size(response.reason_phrase));
}

static void http_response_init_NULL_response_fails(void** state)
{
    (void)state;

    http_response_t response;
    span_t version = HTTP_VERSION_1_1;
    span_t code = HTTP_CODE_200;
    span_t reason_phrase = HTTP_REASON_PHRASE_200;
    http_headers_t headers = { 0 };

    assert_int_equal(http_response_initialize(NULL, version, code, reason_phrase, headers), invalid_argument);
}

static void http_response_init_empty_version_fails(void** state)
{
    (void)state;

    http_response_t response;
    span_t code = HTTP_CODE_200;
    span_t reason_phrase = HTTP_REASON_PHRASE_200;
    http_headers_t headers = { 0 };

    assert_int_equal(http_response_initialize(&response, SPAN_EMPTY, code, reason_phrase, headers), invalid_argument);
}

static void http_response_init_empty_code_fails(void** state)
{
    (void)state;

    http_response_t response;
    span_t version = HTTP_VERSION_1_1;
    span_t reason_phrase = HTTP_REASON_PHRASE_200;
    http_headers_t headers = { 0 };

    assert_int_equal(http_response_initialize(&response, version, SPAN_EMPTY, reason_phrase, headers), invalid_argument);
}

static void http_response_init_empty_reason_phrase_fails(void** state)
{
    (void)state;

    http_response_t response;
    span_t version = HTTP_VERSION_1_1;
    span_t code = HTTP_CODE_200;
    http_headers_t headers = { 0 };

    assert_int_equal(http_response_initialize(&response, version, code, SPAN_EMPTY, headers), invalid_argument);
}

static void http_response_get_http_version_from_init_succeed(void** state)
{
    (void)state;

    http_response_t response;
    span_t version;
    http_headers_t headers = { 0 };

    assert_int_equal(http_response_initialize(&response, HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200, headers), ok);
    assert_int_equal(http_response_get_http_version(response, &version), ok);
    assert_int_equal(span_compare(version, HTTP_VERSION_1_1), 0);
}

static void http_response_get_http_version_empty_version_fails(void** state)
{
    (void)state;

    http_response_t response;
    http_headers_t headers = { 0 };

    assert_int_equal(http_response_initialize(&response, HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200, headers), ok);
    assert_int_equal(http_response_get_http_version(response, NULL), invalid_argument);
}

static void http_response_get_code_from_init_succeed(void** state)
{
    (void)state;

    http_response_t response;
    span_t code;
    http_headers_t headers = { 0 };

    assert_int_equal(http_response_initialize(&response, HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200, headers), ok);
    assert_int_equal(http_response_get_code(response, &code), ok);
    assert_int_equal(span_compare(code, HTTP_CODE_200), 0);
}

static void http_response_get_code_NULL_code_fails(void** state)
{
    (void)state;

    http_response_t response;
    http_headers_t headers = { 0 };

    assert_int_equal(http_response_initialize(&response, HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200, headers), ok);
    assert_int_equal(http_response_get_code(response, NULL), invalid_argument);
}

static void http_response_get_reason_phrase_from_init_succeed(void** state)
{
    (void)state;

    http_response_t response;
    span_t reason_phrase;
    http_headers_t headers = { 0 };

    assert_int_equal(http_response_initialize(&response, HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200, headers), ok);
    assert_int_equal(http_response_get_reason_phrase(response, &reason_phrase), ok);
    assert_int_equal(span_compare(reason_phrase, HTTP_REASON_PHRASE_200), 0);
}

static void http_response_get_reason_phrase_empty_reason_phrase_fails(void** state)
{
    (void)state;

    http_response_t response;
    http_headers_t headers = { 0 };

    assert_int_equal(http_response_initialize(&response, HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200, headers), ok);
    assert_int_equal(http_response_get_reason_phrase(response, NULL), invalid_argument);
}

int test_http_response()
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(http_response_init_succeed),
    cmocka_unit_test(http_response_init_NULL_response_fails),
    cmocka_unit_test(http_response_init_empty_version_fails),
    cmocka_unit_test(http_response_init_empty_code_fails),
    cmocka_unit_test(http_response_init_empty_reason_phrase_fails),
    cmocka_unit_test(http_response_get_http_version_from_init_succeed),
    cmocka_unit_test(http_response_get_http_version_empty_version_fails),
    cmocka_unit_test(http_response_get_code_from_init_succeed),
    cmocka_unit_test(http_response_get_code_NULL_code_fails),
    cmocka_unit_test(http_response_get_reason_phrase_from_init_succeed),
    cmocka_unit_test(http_response_get_reason_phrase_empty_reason_phrase_fails)
  };

  return cmocka_run_group_tests_name("http_response", tests, NULL, NULL);
}