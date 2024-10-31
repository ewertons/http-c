#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include <inttypes.h>
#include "niceties.h"
#include "http_response.h"
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

    span_t buffer = span_from_memory(raw_buffer);
    http_response_t response;
    span_t version = HTTP_VERSION_1_1;
    span_t code = HTTP_CODE_200;
    span_t reason_phrase = HTTP_REASON_PHRASE_200;

    assert_int_equal(http_response_init(&response, buffer, version, code, reason_phrase), ok);
    assert_memory_equal(raw_buffer, TEST_HTTP_RESPONSE_200_OK_1, 17);
}

static void http_response_init_NULL_response_fails(void** state)
{
    (void)state;

    span_t buffer = span_from_memory(raw_buffer);
    http_response_t response;
    span_t version = HTTP_VERSION_1_1;
    span_t code = HTTP_CODE_200;
    span_t reason_phrase = HTTP_REASON_PHRASE_200;

    assert_int_equal(http_response_init(NULL, buffer, version, code, reason_phrase), invalid_argument);
}

static void http_response_init_empty_buffer_fails(void** state)
{
    (void)state;

    http_response_t response;
    span_t version = HTTP_VERSION_1_1;
    span_t code = HTTP_CODE_200;
    span_t reason_phrase = HTTP_REASON_PHRASE_200;

    assert_int_equal(http_response_init(&response, SPAN_EMPTY, version, code, reason_phrase), invalid_argument);
}

static void http_response_init_insufficient_buffer_fails(void** state)
{
    (void)state;

    span_t buffer = span_from_memory(raw_buffer);
    http_response_t response;
    span_t version = HTTP_VERSION_1_1;
    span_t code = HTTP_CODE_200;
    span_t reason_phrase = HTTP_REASON_PHRASE_200;

    assert_int_equal(http_response_init(&response, span_slice(buffer, 0, 10), version, code, reason_phrase), insufficient_size);
}

static void http_response_init_empty_version_fails(void** state)
{
    (void)state;

    span_t buffer = span_from_memory(raw_buffer);
    http_response_t response;
    span_t code = HTTP_CODE_200;
    span_t reason_phrase = HTTP_REASON_PHRASE_200;

    assert_int_equal(http_response_init(&response, buffer, SPAN_EMPTY, code, reason_phrase), invalid_argument);
}

static void http_response_init_empty_code_fails(void** state)
{
    (void)state;

    span_t buffer = span_from_memory(raw_buffer);
    http_response_t response;
    span_t version = HTTP_VERSION_1_1;
    span_t reason_phrase = HTTP_REASON_PHRASE_200;

    assert_int_equal(http_response_init(&response, buffer, version, SPAN_EMPTY, reason_phrase), invalid_argument);
}

static void http_response_init_empty_reason_phrase_fails(void** state)
{
    (void)state;

    span_t buffer = span_from_memory(raw_buffer);
    http_response_t response;
    span_t version = HTTP_VERSION_1_1;
    span_t code = HTTP_CODE_200;

    assert_int_equal(http_response_init(&response, buffer, version, code, SPAN_EMPTY), invalid_argument);
}

static void http_response_get_http_version_from_init_succeed(void** state)
{
    (void)state;

    http_response_t response;
    span_t version;

    assert_int_equal(http_response_init(&response, span_from_memory(raw_buffer), HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200), ok);
    assert_int_equal(http_response_get_http_version(response, &version), ok);
    assert_int_equal(span_compare(version, HTTP_VERSION_1_1), 0);
}

static void http_response_get_http_version_empty_version_fails(void** state)
{
    (void)state;

    http_response_t response;

    assert_int_equal(http_response_init(&response, span_from_memory(raw_buffer), HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200), ok);
    assert_int_equal(http_response_get_http_version(response, NULL), invalid_argument);
}

static void http_response_get_code_from_init_succeed(void** state)
{
    (void)state;

    http_response_t response;
    span_t code;

    assert_int_equal(http_response_init(&response, span_from_memory(raw_buffer), HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200), ok);
    assert_int_equal(http_response_get_code(response, &code), ok);
    assert_int_equal(span_compare(code, HTTP_CODE_200), 0);
}

static void http_response_get_code_NULL_code_fails(void** state)
{
    (void)state;

    http_response_t response;

    assert_int_equal(http_response_init(&response, span_from_memory(raw_buffer), HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200), ok);
    assert_int_equal(http_response_get_code(response, NULL), invalid_argument);
}

static void http_response_get_reason_phrase_from_init_succeed(void** state)
{
    (void)state;

    http_response_t response;
    span_t reason_phrase;

    assert_int_equal(http_response_init(&response, span_from_memory(raw_buffer), HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200), ok);
    assert_int_equal(http_response_get_reason_phrase(response, &reason_phrase), ok);
    assert_int_equal(span_compare(reason_phrase, HTTP_REASON_PHRASE_200), 0);
}

static void http_response_get_reason_phrase_empty_reason_phrase_fails(void** state)
{
    (void)state;

    http_response_t response;

    assert_int_equal(http_response_init(&response, span_from_memory(raw_buffer), HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200), ok);
    assert_int_equal(http_response_get_reason_phrase(response, NULL), invalid_argument);
}

int test_http_response()
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(http_response_init_succeed),
    cmocka_unit_test(http_response_init_NULL_response_fails),
    cmocka_unit_test(http_response_init_empty_buffer_fails),
    cmocka_unit_test(http_response_init_insufficient_buffer_fails),
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