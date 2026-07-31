#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <http_headers.h>
#include <test_http.h>

#define CRLF "\r\n"
#define NVSEP ": "

#define HEADER_NAME_1 "Accept-Encoding"
#define HEADER_NAME_2 "Connection"
#define HEADER_NAME_3 "Upgrade-Insecure-Requests"

#define HEADER_VALUE_1 "gzip, deflate, br"
#define HEADER_VALUE_2 "keep-alive"
#define HEADER_VALUE_3 "1"

static uint8_t headers_str[] =
    HEADER_NAME_1 NVSEP HEADER_VALUE_1 CRLF
        HEADER_NAME_2 NVSEP HEADER_VALUE_2 CRLF
            HEADER_NAME_3 NVSEP HEADER_VALUE_3 CRLF
                CRLF;

static const span_t header_name_1 = span_from_str_literal(HEADER_NAME_1);
static const span_t header_name_2 = span_from_str_literal(HEADER_NAME_2);
static const span_t header_name_3 = span_from_str_literal(HEADER_NAME_3);
static const span_t header_value_1 = span_from_str_literal(HEADER_VALUE_1);
static const span_t header_value_2 = span_from_str_literal(HEADER_VALUE_2);
static const span_t header_value_3 = span_from_str_literal(HEADER_VALUE_3);

static void http_headers_init_succeed(void **state)
{
  (void)state;

  http_headers_t headers;
  span_t name, value;

  span_t b = span_from_string(headers_str);

  assert_int_equal(http_headers_init(&headers, b), HL_RESULT_OK);
  assert_int_equal(http_headers_get_next(&headers, &name, &value), HL_RESULT_EOF);
}

static void http_headers_parse_succeed(void **state)
{
  (void)state;

  http_headers_t headers;
  span_t name, value;

  span_t b = span_from_string(headers_str);

  assert_int_equal(http_headers_parse(&headers, b), ok);

  assert_int_equal(http_headers_get_next(&headers, &name, &value), HL_RESULT_OK);
  assert_int_equal(span_compare(name, header_name_1), 0);
  assert_int_equal(span_compare(value, header_value_1), 0);

  assert_int_equal(http_headers_get_next(&headers, &name, &value), HL_RESULT_OK);
  assert_int_equal(span_compare(name, header_name_2), 0);
  assert_int_equal(span_compare(value, header_value_2), 0);

  assert_int_equal(http_headers_get_next(&headers, &name, &value), HL_RESULT_OK);
  assert_int_equal(span_compare(name, header_name_3), 0);
  assert_int_equal(span_compare(value, header_value_3), 0);

  assert_int_equal(http_headers_get_next(&headers, &name, &value), HL_RESULT_EOF);
}

static void http_headers_get_next_succeed(void **state)
{
  (void)state;

  http_headers_t headers;
  span_t name, value;

  span_t b = span_from_string(headers_str);

  assert_int_equal(http_headers_parse(&headers, b), ok);

  for (uint32_t n = 0; n < 100; n++)
  {
    for (uint32_t i = 0; i < 3; i++)
    {
      assert_int_equal(http_headers_get_next(&headers, &name, &value), HL_RESULT_OK);
    }

    assert_int_equal(http_headers_get_next(&headers, &name, &value), HL_RESULT_EOF);
  }
}

static void http_headers_get_name_and_value_succeed(void **state)
{
  (void)state;

  http_headers_t headers;
  span_t name, value;

  span_t b = span_from_string(headers_str);

  assert_int_equal(http_headers_parse(&headers, b), ok);

  for (uint32_t n = 0; n < 3; n++)
  {
    for (uint32_t i = 0; i < 3; i++)
    {
      assert_int_equal(http_headers_get_next(&headers, &name, &value), HL_RESULT_OK);

      switch (i)
      {
      case 0:
        assert_int_equal(span_get_size(name), strlitlen(HEADER_NAME_1));
        assert_ptr_equal(span_get_ptr(name), span_get_ptr(b));
        assert_int_equal(span_get_size(value), strlitlen(HEADER_VALUE_1));
        assert_ptr_equal(span_get_ptr(value), span_get_ptr(b) + strlitlen(HEADER_NAME_1) + strlitlen(NVSEP));
        break;
      case 1:
        assert_int_equal(span_get_size(name), strlitlen(HEADER_NAME_2));
        assert_ptr_equal(span_get_ptr(name), span_get_ptr(b) + 
          strlitlen(HEADER_NAME_1) + strlitlen(NVSEP) + strlitlen(HEADER_VALUE_1) + strlitlen(CRLF));
        assert_int_equal(span_get_size(value), strlitlen(HEADER_VALUE_2));
        assert_ptr_equal(span_get_ptr(value), span_get_ptr(b) + 
          strlitlen(HEADER_NAME_1) + strlitlen(NVSEP) + strlitlen(HEADER_VALUE_1) + strlitlen(CRLF) + 
          strlitlen(HEADER_NAME_2) + strlitlen(NVSEP));
        break;
      case 2:
        assert_int_equal(span_get_size(name), strlitlen(HEADER_NAME_3));
        assert_ptr_equal(span_get_ptr(name), span_get_ptr(b) + 
          strlitlen(HEADER_NAME_1) + strlitlen(NVSEP) + strlitlen(HEADER_VALUE_1) + strlitlen(CRLF) +
          strlitlen(HEADER_NAME_2) + strlitlen(NVSEP) + strlitlen(HEADER_VALUE_2) + strlitlen(CRLF));
        assert_int_equal(span_get_size(value), strlitlen(HEADER_VALUE_3));
        assert_ptr_equal(span_get_ptr(value), span_get_ptr(b) + 
          strlitlen(HEADER_NAME_1) + strlitlen(NVSEP) + strlitlen(HEADER_VALUE_1) + strlitlen(CRLF) + 
          strlitlen(HEADER_NAME_2) + strlitlen(NVSEP) + strlitlen(HEADER_VALUE_2) + strlitlen(CRLF) +
          strlitlen(HEADER_NAME_3) + strlitlen(NVSEP));
        break;
      default:
        assert_true(false);
        break;
      }
    }

    assert_int_equal(http_headers_get_next(&headers, &name, &value), HL_RESULT_EOF);
  }
}

static void http_headers_get_buffer_succeed(void **state)
{
  (void)state;

  http_headers_t headers;
  http_headers_t header;
  span_t name, value;

  span_t b = span_from_string(headers_str);
  span_t c;

  /* The header block ends with CRLFCRLF. http_headers_parse trims at the
   * second CRLF so the returned buffer is the actual header content
   * (everything up to and including the trailing CRLF of the last header). */
  span_t expected = span_slice(b, 0, span_get_size(b) - strlitlen(CRLF));

  assert_int_equal(http_headers_parse(&headers, b), ok);
  assert_int_equal(http_headers_get_buffer(&headers, &c), HL_RESULT_OK);
  assert_int_equal(span_compare(c, expected), 0);
}

static void http_headers_add_succeed(void **state)
{
  (void)state;

  http_headers_t headers;
  uint8_t raw_buffer[100];
  span_t out_buffer;
  span_t expected_content = span_from_str_literal(
    HEADER_NAME_3 NVSEP HEADER_VALUE_2 CRLF
    HEADER_NAME_1 NVSEP HEADER_VALUE_3 CRLF
    HEADER_NAME_2 NVSEP HEADER_VALUE_1 CRLF
  );

  span_t b = span_from_memory(raw_buffer);

  assert_int_equal(http_headers_init(&headers, b), HL_RESULT_OK);
  assert_int_equal(http_headers_add(&headers, header_name_3, header_value_2), HL_RESULT_OK);
  assert_int_equal(http_headers_add(&headers, header_name_1, header_value_3), HL_RESULT_OK);
  assert_int_equal(http_headers_add(&headers, header_name_2, header_value_1), HL_RESULT_OK);

  assert_int_equal(http_headers_get_buffer(&headers, &out_buffer), HL_RESULT_OK);
  assert_int_equal(span_get_size(out_buffer), span_get_size(expected_content));
  assert_memory_equal(span_get_ptr(out_buffer), span_get_ptr(expected_content), span_get_size(expected_content));
}

static void http_headers_add_overflow_fail(void **state)
{
  (void)state;

  http_headers_t headers;
  uint8_t raw_buffer[10];

  span_t b = span_from_memory(raw_buffer);

  assert_int_equal(http_headers_init(&headers, b), HL_RESULT_OK);
  assert_int_equal(http_headers_add(&headers, header_name_3, header_value_2), HL_RESULT_BUFFER_OVERFLOW);
}

/* Everything below is about http_headers_find, which had no coverage at
 * all -- and the reason the gap mattered is that the whole test suite
 * writes headers with the same literals it reads them back with, so a
 * lookup that only worked for http-c's own spelling could never fail a
 * test. Each of these is written the way a client on the wire writes it,
 * not the way this library does. */

static void find_headers(const char* raw, span_t name, span_t* value, HL_RESULT expected)
{
  http_headers_t headers;
  span_t         b = span_init((uint8_t*)(uintptr_t)raw, (uint32_t)strlen(raw));

  assert_int_equal(http_headers_parse(&headers, b), ok);
  assert_int_equal(http_headers_find(&headers, name, value), expected);
}

static void http_headers_find_succeed(void **state)
{
  (void)state;

  span_t value;
  find_headers(HEADER_NAME_1 NVSEP HEADER_VALUE_1 CRLF
               HEADER_NAME_2 NVSEP HEADER_VALUE_2 CRLF CRLF,
               header_name_2, &value, HL_RESULT_OK);
  assert_int_equal(span_compare(value, header_value_2), 0);
}

/* RFC 7230 3.2: field names are case-insensitive. Node, Playwright and
 * every HTTP/2 client send them lowercased, and reading them byte-exact
 * meant Content-Length was never found -- so the parser waited for zero
 * body bytes and handed the handler an empty body. */
static void http_headers_find_is_case_insensitive(void **state)
{
  (void)state;

  span_t value;
  const span_t content_length = span_from_str_literal("Content-Length");

  find_headers("content-length: 42" CRLF CRLF, content_length, &value, HL_RESULT_OK);
  assert_int_equal(span_compare(value, span_from_str_literal("42")), 0);

  find_headers("CONTENT-LENGTH: 42" CRLF CRLF, content_length, &value, HL_RESULT_OK);
  assert_int_equal(span_compare(value, span_from_str_literal("42")), 0);

  find_headers("CoNtEnT-lEnGtH: 42" CRLF CRLF, content_length, &value, HL_RESULT_OK);
  assert_int_equal(span_compare(value, span_from_str_literal("42")), 0);
}

/* RFC 7230 3.2: the colon follows the name immediately and the whitespace
 * around the value is optional. Requiring the single space meant a client
 * that omitted it hit the same silently-empty-body path. */
static void http_headers_find_tolerates_whitespace(void **state)
{
  (void)state;

  span_t value;
  const span_t expected = span_from_str_literal("42");
  const span_t content_length = span_from_str_literal("Content-Length");

  find_headers("Content-Length:42" CRLF CRLF, content_length, &value, HL_RESULT_OK);
  assert_int_equal(span_compare(value, expected), 0);

  find_headers("Content-Length:   42" CRLF CRLF, content_length, &value, HL_RESULT_OK);
  assert_int_equal(span_compare(value, expected), 0);

  find_headers("Content-Length:\t42\t " CRLF CRLF, content_length, &value, HL_RESULT_OK);
  assert_int_equal(span_compare(value, expected), 0);
}

/* A value may legitimately contain a colon -- Host, Date, Referer -- so
 * only the first one separates. */
static void http_headers_find_splits_on_first_colon(void **state)
{
  (void)state;

  span_t value;
  find_headers("Host: example.com:8080" CRLF CRLF,
               span_from_str_literal("Host"), &value, HL_RESULT_OK);
  assert_int_equal(span_compare(value, span_from_str_literal("example.com:8080")), 0);
}

/* One unparseable line used to be reported as an error for the whole
 * lookup, which hid every header after it. */
static void http_headers_find_skips_malformed_line(void **state)
{
  (void)state;

  span_t value;
  find_headers("this-line-has-no-colon" CRLF
               HEADER_NAME_2 NVSEP HEADER_VALUE_2 CRLF CRLF,
               header_name_2, &value, HL_RESULT_OK);
  assert_int_equal(span_compare(value, header_value_2), 0);
}

static void http_headers_find_missing_returns_not_found(void **state)
{
  (void)state;

  span_t value;
  find_headers(HEADER_NAME_1 NVSEP HEADER_VALUE_1 CRLF CRLF,
               span_from_str_literal("X-Not-Here"), &value, HL_RESULT_NOT_FOUND);
}

int test_http_headers()
{
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(http_headers_init_succeed),
      cmocka_unit_test(http_headers_parse_succeed),
      cmocka_unit_test(http_headers_get_next_succeed),
      cmocka_unit_test(http_headers_get_name_and_value_succeed),
      cmocka_unit_test(http_headers_add_succeed),
      cmocka_unit_test(http_headers_get_buffer_succeed),
      cmocka_unit_test(http_headers_add_overflow_fail),
      cmocka_unit_test(http_headers_find_succeed),
      cmocka_unit_test(http_headers_find_is_case_insensitive),
      cmocka_unit_test(http_headers_find_tolerates_whitespace),
      cmocka_unit_test(http_headers_find_splits_on_first_colon),
      cmocka_unit_test(http_headers_find_skips_malformed_line),
      cmocka_unit_test(http_headers_find_missing_returns_not_found)
      };

  return cmocka_run_group_tests_name("http_headers", tests, NULL, NULL);
}