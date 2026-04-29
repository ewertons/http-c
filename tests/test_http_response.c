#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include <inttypes.h>
#include <string.h>

#include "niceties.h"

#include "http_response.h"
#include "http_versions.h"
#include "http_codes.h"
#include "stream.h"

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

/* ----- Memory-backed stream helper ----- */
typedef struct mem_sink
{
    uint8_t* buffer;
    uint32_t capacity;
    uint32_t written;
} mem_sink_t;

static result_t mem_open(struct stream* s)  { (void)s; return ok; }
static result_t mem_close(struct stream* s) { (void)s; return ok; }

static result_t mem_write(struct stream* inner_stream, span_t data, span_t* remainder)
{
    (void)remainder;
    mem_sink_t* sink = (mem_sink_t*)inner_stream;
    uint32_t n = span_get_size(data);
    if (sink->written + n > sink->capacity) return error;
    memcpy(sink->buffer + sink->written, span_get_ptr(data), n);
    sink->written += n;
    return ok;
}

static result_t mem_read(struct stream* s, span_t b, span_t* r, span_t* rem)
{
    (void)s; (void)b; (void)r; (void)rem;
    return error;
}

static void mem_stream_init(stream_t* stream, mem_sink_t* sink)
{
    stream->open = mem_open;
    stream->close = mem_close;
    stream->write = mem_write;
    stream->read = mem_read;
    stream->inner_stream = sink;
}

static void http_response_set_get_body_round_trip_succeed(void** state)
{
    (void)state;

    http_response_t response;
    http_headers_t headers = { 0 };
    assert_int_equal(ok, http_response_initialize(&response, HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200, headers));

    span_t body = span_from_str_literal("hello body");
    assert_int_equal(ok, http_response_set_body(&response, body));

    span_t out;
    assert_int_equal(ok, http_response_get_body(&response, &out));
    assert_int_equal(span_get_size(body), span_get_size(out));
    assert_memory_equal(span_get_ptr(body), span_get_ptr(out), span_get_size(body));
}

static void http_response_set_body_null_response_fails(void** state)
{
    (void)state;
    assert_int_equal(invalid_argument,
                     http_response_set_body(NULL, span_from_str_literal("x")));
}

static void http_response_get_body_null_args_fail(void** state)
{
    (void)state;
    http_response_t response;
    span_t out;
    assert_int_equal(invalid_argument, http_response_get_body(NULL, &out));
    assert_int_equal(invalid_argument, http_response_get_body(&response, NULL));
}

static void http_response_serialize_to_succeed(void** state)
{
    (void)state;

    http_response_t response;
    http_headers_t headers = { 0 };
    assert_int_equal(ok, http_response_initialize(&response, HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200, headers));

    uint8_t buffer[256];
    mem_sink_t sink = { buffer, sizeof(buffer), 0 };
    stream_t stream;
    mem_stream_init(&stream, &sink);

    assert_int_equal(ok, http_response_serialize_to(&response, &stream));

    const char* expected_start = "HTTP/1.1 200 OK\r\n";
    assert_true(sink.written >= strlen(expected_start));
    assert_memory_equal(buffer, expected_start, strlen(expected_start));
    assert_true(sink.written >= 2);
    assert_int_equal('\r', buffer[sink.written - 2]);
    assert_int_equal('\n', buffer[sink.written - 1]);
}

static void http_response_serialize_to_null_args_fail(void** state)
{
    (void)state;
    http_response_t response;
    stream_t stream;
    assert_int_equal(invalid_argument, http_response_serialize_to(NULL,    &stream));
    assert_int_equal(invalid_argument, http_response_serialize_to(&response, NULL));
}

static void http_response_parse_succeed(void** state)
{
    (void)state;

    http_response_t response;
    span_t buffer = span_from_string(TEST_HTTP_RESPONSE_200_OK_1);
    span_t remainder;

    assert_int_equal(ok, http_response_parse(&response, buffer, &remainder));

    span_t version, code, reason;
    assert_int_equal(ok, http_response_get_http_version(response, &version));
    assert_int_equal(0, span_compare(version, HTTP_VERSION_1_1));
    assert_int_equal(ok, http_response_get_code(response, &code));
    assert_int_equal(0, span_compare(code, HTTP_CODE_200));
    assert_int_equal(ok, http_response_get_reason_phrase(response, &reason));
    assert_int_equal(0, span_compare(reason, HTTP_REASON_PHRASE_200));
}

static void http_response_parse_null_args_fail(void** state)
{
    (void)state;
    http_response_t response;
    span_t buffer = span_from_string(TEST_HTTP_RESPONSE_200_OK_1);
    span_t remainder;
    assert_int_not_equal(ok, http_response_parse(NULL, buffer, &remainder));
    (void)response;
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
    cmocka_unit_test(http_response_get_reason_phrase_empty_reason_phrase_fails),
    cmocka_unit_test(http_response_set_get_body_round_trip_succeed),
    cmocka_unit_test(http_response_set_body_null_response_fails),
    cmocka_unit_test(http_response_get_body_null_args_fail),
    cmocka_unit_test(http_response_serialize_to_succeed),
    cmocka_unit_test(http_response_serialize_to_null_args_fail),
    cmocka_unit_test(http_response_parse_succeed),
    cmocka_unit_test(http_response_parse_null_args_fail),
  };

  return cmocka_run_group_tests_name("http_response", tests, NULL, NULL);
}