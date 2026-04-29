#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <inttypes.h>
#include <string.h>

#include <cmocka.h>

#include "niceties.h"
#include <http_request.h>
#include "http_versions.h"
#include "http_methods.h"
#include "stream.h"

#include <test_http.h>

static uint8_t TEST_HTTP_REQUEST_GET_1[] = "GET / HTTP/1.1\r\n\
Host: localhost:1234\r\n\
User-Agent: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:75.0) Gecko/20100101 Firefox/75.0\r\n\
Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8\r\n\
Accept-Language: en-US,en;q=0.5\r\n\
Accept-Encoding: gzip, deflate, br\r\n\
Connection: keep-alive\r\n\
Upgrade-Insecure-Requests: 1\r\n\
\r\n";

static void http_request_get_GET_succeed(void** state)
{
    (void)state;

    http_request_t request;
    span_t method;
    span_t uri;
    span_t version;

    span_t buffer =  span_from_string(TEST_HTTP_REQUEST_GET_1);

    assert_int_equal(http_request_parse(&request, buffer), ok);
    assert_int_equal(http_request_get_method(&request, &method), ok);
    assert_int_equal(span_compare(method, HTTP_METHOD_GET), 0);

    assert_int_equal(http_request_get_path(&request, &uri), ok);
    assert_int_equal(span_compare(uri, span_from_str_literal("/")), 0);

    assert_int_equal(http_request_get_http_version(&request, &version), ok);
    assert_int_equal(span_compare(version, HTTP_VERSION_1_1), 0);
}

static void http_request_get_GET_headers_succeed(void** state)
{
    (void)state;

    http_request_t request;
    http_headers_t headers;
    span_t name, value;

    span_t buffer =  span_from_string(TEST_HTTP_REQUEST_GET_1);

    assert_int_equal(http_request_parse(&request, buffer), ok);

    assert_int_equal(http_request_get_headers(&request, &headers), ok);

    for (int i = 0; i < 7; i++)
    {
      assert_int_equal(http_headers_get_next(&headers, &name, &value), HL_RESULT_OK);

      switch(i)
      {
        case 0:
          assert_int_equal(span_compare(name, span_from_str_literal("Host")), 0);
          assert_int_equal(span_compare(value, span_from_str_literal("localhost:1234")), 0);
          break;
        case 1:
          assert_int_equal(span_compare(name, span_from_str_literal("User-Agent")), 0);
          assert_int_equal(span_compare(value, span_from_str_literal("Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:75.0) Gecko/20100101 Firefox/75.0")), 0);
          break;
        case 2:
          assert_int_equal(span_compare(name, span_from_str_literal("Accept")), 0);
          assert_int_equal(span_compare(value, span_from_str_literal("text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8")), 0);
          break;
        case 3:
          assert_int_equal(span_compare(name, span_from_str_literal("Accept-Language")), 0);
          assert_int_equal(span_compare(value, span_from_str_literal("en-US,en;q=0.5")), 0);
          break;
        case 4:
          assert_int_equal(span_compare(name, span_from_str_literal("Accept-Encoding")), 0);
          assert_int_equal(span_compare(value, span_from_str_literal("gzip, deflate, br")), 0);
          break;
        case 5:
          assert_int_equal(span_compare(name, span_from_str_literal("Connection")), 0);
          assert_int_equal(span_compare(value, span_from_str_literal("keep-alive")), 0);
          break;
        case 6:
          assert_int_equal(span_compare(name, span_from_str_literal("Upgrade-Insecure-Requests")), 0);
          assert_int_equal(span_compare(value, span_from_str_literal("1")), 0);
          break;
        default:
          break;
      }
    }

    assert_int_equal(http_headers_get_next(&headers, &name, &value), HL_RESULT_EOF);
}

/* ----- Memory-backed stream helper for serialize_to tests ----- */
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

static void http_request_initialize_succeed(void** state)
{
    (void)state;

    http_request_t request;
    http_headers_t headers = { 0 };

    assert_int_equal(ok, http_request_initialize(&request,
                                                 HTTP_METHOD_GET,
                                                 span_from_str_literal("/path"),
                                                 HTTP_VERSION_1_1,
                                                 headers));

    span_t method, path, version;
    assert_int_equal(ok, http_request_get_method(&request, &method));
    assert_int_equal(0, span_compare(method, HTTP_METHOD_GET));
    assert_int_equal(ok, http_request_get_path(&request, &path));
    assert_int_equal(0, span_compare(path, span_from_str_literal("/path")));
    assert_int_equal(ok, http_request_get_http_version(&request, &version));
    assert_int_equal(0, span_compare(version, HTTP_VERSION_1_1));
}

static void http_request_initialize_null_request_fails(void** state)
{
    (void)state;
    http_headers_t headers = { 0 };
    assert_int_equal(invalid_argument,
                     http_request_initialize(NULL,
                                             HTTP_METHOD_GET,
                                             span_from_str_literal("/"),
                                             HTTP_VERSION_1_1,
                                             headers));
}

static void http_request_set_get_body_round_trip_succeed(void** state)
{
    (void)state;

    http_request_t request;
    http_headers_t headers = { 0 };
    assert_int_equal(ok, http_request_initialize(&request,
                                                 HTTP_METHOD_POST,
                                                 span_from_str_literal("/x"),
                                                 HTTP_VERSION_1_1,
                                                 headers));

    span_t body = span_from_str_literal("payload-data");
    assert_int_equal(ok, http_request_set_body(&request, body));

    span_t out;
    assert_int_equal(ok, http_request_get_body(&request, &out));
    assert_int_equal(span_get_size(body), span_get_size(out));
    assert_memory_equal(span_get_ptr(body), span_get_ptr(out), span_get_size(body));
}

static void http_request_set_body_null_request_fails(void** state)
{
    (void)state;
    assert_int_equal(invalid_argument,
                     http_request_set_body(NULL, span_from_str_literal("x")));
}

static void http_request_get_body_null_args_fail(void** state)
{
    (void)state;
    http_request_t request;
    span_t out;
    assert_int_equal(invalid_argument, http_request_get_body(NULL, &out));
    assert_int_equal(invalid_argument, http_request_get_body(&request, NULL));
}

static void http_request_serialize_to_succeed(void** state)
{
    (void)state;

    http_request_t request;
    http_headers_t headers = { 0 };
    assert_int_equal(ok, http_request_initialize(&request,
                                                 HTTP_METHOD_GET,
                                                 span_from_str_literal("/foo"),
                                                 HTTP_VERSION_1_1,
                                                 headers));

    uint8_t buffer[256];
    mem_sink_t sink = { buffer, sizeof(buffer), 0 };
    stream_t stream;
    mem_stream_init(&stream, &sink);

    assert_int_equal(ok, http_request_serialize_to(&request, &stream));

    /* The output must start with "GET /foo HTTP/1.1\r\n" and end with the
     * extra CRLF that closes the header block. */
    const char* expected_start = "GET /foo HTTP/1.1\r\n";
    assert_true(sink.written >= strlen(expected_start));
    assert_memory_equal(buffer, expected_start, strlen(expected_start));
    assert_true(sink.written >= 2);
    assert_int_equal('\r', buffer[sink.written - 2]);
    assert_int_equal('\n', buffer[sink.written - 1]);
}

static void http_request_serialize_to_null_args_fail(void** state)
{
    (void)state;
    http_request_t request;
    stream_t stream;
    assert_int_equal(invalid_argument, http_request_serialize_to(NULL,    &stream));
    assert_int_equal(invalid_argument, http_request_serialize_to(&request, NULL));
}

static void http_request_parse_invalid_buffer_fails(void** state)
{
    (void)state;
    http_request_t request;
    /* Garbage with no spaces or CRLFs - cannot parse a request line. */
    span_t bad = span_from_str_literal("not-a-request");
    assert_int_not_equal(ok, http_request_parse(&request, bad));
}

static void http_request_get_method_null_args_fail(void** state)
{
    (void)state;
    http_request_t request;
    span_t method;
    assert_int_equal(invalid_argument, http_request_get_method(NULL, &method));
    assert_int_equal(invalid_argument, http_request_get_method(&request, NULL));
}

int test_http_request()
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(http_request_get_GET_succeed),
    cmocka_unit_test(http_request_get_GET_headers_succeed),
    cmocka_unit_test(http_request_initialize_succeed),
    cmocka_unit_test(http_request_initialize_null_request_fails),
    cmocka_unit_test(http_request_set_get_body_round_trip_succeed),
    cmocka_unit_test(http_request_set_body_null_request_fails),
    cmocka_unit_test(http_request_get_body_null_args_fail),
    cmocka_unit_test(http_request_serialize_to_succeed),
    cmocka_unit_test(http_request_serialize_to_null_args_fail),
    cmocka_unit_test(http_request_parse_invalid_buffer_fails),
    cmocka_unit_test(http_request_get_method_null_args_fail),
  };

  return cmocka_run_group_tests_name("http_request", tests, NULL, NULL);
}