#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include <inttypes.h>
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

  assert_int_equal(http_headers_parse(&headers, b), HL_RESULT_OK);

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

  assert_int_equal(http_headers_parse(&headers, b), HL_RESULT_OK);

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

  assert_int_equal(http_headers_parse(&headers, b), HL_RESULT_OK);

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

  assert_int_equal(http_headers_parse(&headers, b), HL_RESULT_OK);
  assert_int_equal(http_headers_get_buffer(&headers, &c), HL_RESULT_OK);
  assert_int_equal(span_compare(c, b), 0);
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

int test_http_headers()
{
  const struct CMUnitTest tests[] = {
      cmocka_unit_test(http_headers_init_succeed),
      cmocka_unit_test(http_headers_parse_succeed),
      cmocka_unit_test(http_headers_get_next_succeed),
      cmocka_unit_test(http_headers_get_name_and_value_succeed),
      cmocka_unit_test(http_headers_add_succeed),
      cmocka_unit_test(http_headers_get_buffer_succeed),
      cmocka_unit_test(http_headers_add_overflow_fail)
      };

  return cmocka_run_group_tests_name("http_headers", tests, NULL, NULL);
}