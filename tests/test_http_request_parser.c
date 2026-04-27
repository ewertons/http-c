#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include <span.h>
#include "niceties.h"

#include "http_request_parser.h"
#include "http_headers.h"

/* Convenience: feed the parser a NUL-terminated literal. */
static result_t feed_literal(http_request_parser_t* p, const char* s)
{
    span_t buf = span_init((uint8_t*)s, (uint32_t)strlen(s));
    return http_request_parser_feed(p, buf);
}

/* ---------- positive cases ---------- */

static void parser_completes_simple_get(void** state)
{
    (void)state;
    http_request_parser_t p;
    http_request_parser_init(&p);

    assert_int_equal(feed_literal(&p, "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n"), ok);
    assert_int_equal(http_request_parser_get_state(&p),
                     http_request_parser_state_complete);

    http_request_t* req = http_request_parser_get_request(&p);
    assert_int_equal(span_compare(req->method,       span_from_str_literal("GET")), 0);
    assert_int_equal(span_compare(req->path,         span_from_str_literal("/index.html")), 0);
    assert_int_equal(span_compare(req->http_version, span_from_str_literal("HTTP/1.1")), 0);
    assert_true(span_is_empty(req->body));
}

static void parser_completes_post_with_body(void** state)
{
    (void)state;
    http_request_parser_t p;
    http_request_parser_init(&p);

    const char* msg =
        "POST /api HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    assert_int_equal(feed_literal(&p, msg), ok);
    assert_int_equal(http_request_parser_get_state(&p),
                     http_request_parser_state_complete);

    http_request_t* req = http_request_parser_get_request(&p);
    assert_int_equal(span_compare(req->body, span_from_str_literal("hello")), 0);
    assert_int_equal(http_request_parser_get_consumed(&p), strlen(msg));
}

static void parser_handles_byte_at_a_time(void** state)
{
    (void)state;
    http_request_parser_t p;
    http_request_parser_init(&p);

    const char* msg =
        "POST /a HTTP/1.1\r\n"
        "Content-Length: 3\r\n"
        "\r\n"
        "abc";
    size_t total = strlen(msg);

    /* Feed N bytes at a time, growing buffer up to total length. */
    for (size_t i = 1; i < total; i++)
    {
        span_t buf = span_init((uint8_t*)msg, (uint32_t)i);
        result_t r = http_request_parser_feed(&p, buf);
        assert_int_equal(r, try_again);
        assert_int_not_equal(http_request_parser_get_state(&p),
                             http_request_parser_state_complete);
        assert_int_not_equal(http_request_parser_get_state(&p),
                             http_request_parser_state_error);
    }

    span_t buf = span_init((uint8_t*)msg, (uint32_t)total);
    assert_int_equal(http_request_parser_feed(&p, buf), ok);
    assert_int_equal(http_request_parser_get_state(&p),
                     http_request_parser_state_complete);

    http_request_t* req = http_request_parser_get_request(&p);
    assert_int_equal(span_compare(req->body, span_from_str_literal("abc")), 0);
}

static void parser_returns_try_again_when_body_truncated(void** state)
{
    (void)state;
    http_request_parser_t p;
    http_request_parser_init(&p);

    /* Headers say 10 bytes but only 4 supplied. */
    const char* msg =
        "POST / HTTP/1.1\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "abcd";

    assert_int_equal(feed_literal(&p, msg), try_again);
    assert_int_equal(http_request_parser_get_state(&p),
                     http_request_parser_state_body);
}

/* ---------- negative cases ---------- */

static void parser_init_with_null_does_not_crash(void** state)
{
    (void)state;
    http_request_parser_init(NULL); /* no-op, must not crash */
}

static void parser_feed_with_null_returns_invalid_argument(void** state)
{
    (void)state;
    span_t empty = SPAN_EMPTY;
    assert_int_equal(http_request_parser_feed(NULL, empty), invalid_argument);
}

static void parser_returns_try_again_on_empty_buffer(void** state)
{
    (void)state;
    http_request_parser_t p;
    http_request_parser_init(&p);
    span_t empty = SPAN_EMPTY;
    assert_int_equal(http_request_parser_feed(&p, empty), try_again);
}

static void parser_rejects_invalid_content_length(void** state)
{
    (void)state;
    http_request_parser_t p;
    http_request_parser_init(&p);

    const char* msg =
        "POST / HTTP/1.1\r\n"
        "Content-Length: notanumber\r\n"
        "\r\n";

    assert_int_equal(feed_literal(&p, msg), error);
    assert_int_equal(http_request_parser_get_state(&p),
                     http_request_parser_state_error);
}

int test_http_request_parser()
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(parser_completes_simple_get),
        cmocka_unit_test(parser_completes_post_with_body),
        cmocka_unit_test(parser_handles_byte_at_a_time),
        cmocka_unit_test(parser_returns_try_again_when_body_truncated),
        cmocka_unit_test(parser_init_with_null_does_not_crash),
        cmocka_unit_test(parser_feed_with_null_returns_invalid_argument),
        cmocka_unit_test(parser_returns_try_again_on_empty_buffer),
        cmocka_unit_test(parser_rejects_invalid_content_length),
    };
    return cmocka_run_group_tests_name("http_request_parser", tests, NULL, NULL);
}
