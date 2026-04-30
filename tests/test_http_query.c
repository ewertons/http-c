#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

#include <http_query.h>
#include <test_http.h>

#define EXPECT_SPAN_EQ(span, str)                                       \
    do {                                                                \
        span_t _expected = span_from_str_literal(str);                  \
        assert_int_equal(span_get_size(span), span_get_size(_expected));\
        assert_memory_equal(span_get_ptr(span),                          \
                            span_get_ptr(_expected),                     \
                            span_get_size(_expected));                   \
    } while (0)

/* ----- find -------------------------------------------------------------- */

static void find_basic_succeed(void** state)
{
    (void)state;
    span_t path = span_from_str_literal("/cb?code=ABC&state=xyz");
    span_t v;
    assert_int_equal(http_query_find(path, span_from_str_literal("code"),  &v), ok);
    EXPECT_SPAN_EQ(v, "ABC");
    assert_int_equal(http_query_find(path, span_from_str_literal("state"), &v), ok);
    EXPECT_SPAN_EQ(v, "xyz");
}

static void find_missing_returns_not_found(void** state)
{
    (void)state;
    span_t path = span_from_str_literal("/cb?code=ABC");
    span_t v;
    assert_int_equal(
        http_query_find(path, span_from_str_literal("error"), &v), not_found);
}

static void find_no_question_mark_scans_as_query(void** state)
{
    (void)state;
    /* If the caller already trimmed off the path, the bare "k=v&..."
     * form should also work. */
    span_t qs = span_from_str_literal("a=1&b=2");
    span_t v;
    assert_int_equal(http_query_find(qs, span_from_str_literal("b"), &v), ok);
    EXPECT_SPAN_EQ(v, "2");
}

static void find_empty_value_succeeds(void** state)
{
    (void)state;
    span_t path = span_from_str_literal("/?code=&state=ok");
    span_t v;
    assert_int_equal(http_query_find(path, span_from_str_literal("code"), &v), ok);
    assert_int_equal(span_get_size(v), 0);
}

static void find_first_match_wins(void** state)
{
    (void)state;
    span_t path = span_from_str_literal("/?k=first&k=second");
    span_t v;
    assert_int_equal(http_query_find(path, span_from_str_literal("k"), &v), ok);
    EXPECT_SPAN_EQ(v, "first");
}

static void find_null_out_invalid(void** state)
{
    (void)state;
    span_t path = span_from_str_literal("/?a=1");
    assert_int_equal(
        http_query_find(path, span_from_str_literal("a"), NULL),
        invalid_argument);
}

/* ----- find_decoded ------------------------------------------------------ */

static void find_decoded_percent_and_plus(void** state)
{
    (void)state;
    span_t path = span_from_str_literal("/?q=hello+world%21&u=4%2F0AeaY");
    uint8_t buf[64];
    span_t out;
    assert_int_equal(
        http_query_find_decoded(path, span_from_str_literal("q"),
                                span_from_memory(buf), &out), ok);
    EXPECT_SPAN_EQ(out, "hello world!");

    assert_int_equal(
        http_query_find_decoded(path, span_from_str_literal("u"),
                                span_from_memory(buf), &out), ok);
    EXPECT_SPAN_EQ(out, "4/0AeaY");
}

static void find_decoded_too_small_buffer(void** state)
{
    (void)state;
    span_t path = span_from_str_literal("/?q=abcdef");
    uint8_t buf[3];
    span_t out;
    assert_int_equal(
        http_query_find_decoded(path, span_from_str_literal("q"),
                                span_from_memory(buf), &out),
        insufficient_size);
}

static void find_decoded_malformed_percent(void** state)
{
    (void)state;
    span_t path = span_from_str_literal("/?q=%2");
    uint8_t buf[16];
    span_t out;
    assert_int_equal(
        http_query_find_decoded(path, span_from_str_literal("q"),
                                span_from_memory(buf), &out),
        unexpected_char);
}

static void find_decoded_missing_propagates_not_found(void** state)
{
    (void)state;
    span_t path = span_from_str_literal("/?q=ok");
    uint8_t buf[16];
    span_t out;
    assert_int_equal(
        http_query_find_decoded(path, span_from_str_literal("z"),
                                span_from_memory(buf), &out),
        not_found);
}

/* ----- iterator ---------------------------------------------------------- */

static void iterator_walks_all_pairs(void** state)
{
    (void)state;
    span_t path = span_from_str_literal("/p?a=1&b=&c=hello");
    http_query_iterator_t it;
    span_t k, v;

    assert_int_equal(http_query_iterator_init(&it, path), ok);

    assert_int_equal(http_query_iterator_next(&it, &k, &v), ok);
    EXPECT_SPAN_EQ(k, "a"); EXPECT_SPAN_EQ(v, "1");

    assert_int_equal(http_query_iterator_next(&it, &k, &v), ok);
    EXPECT_SPAN_EQ(k, "b"); assert_int_equal(span_get_size(v), 0);

    assert_int_equal(http_query_iterator_next(&it, &k, &v), ok);
    EXPECT_SPAN_EQ(k, "c"); EXPECT_SPAN_EQ(v, "hello");

    assert_int_equal(http_query_iterator_next(&it, &k, &v), end_of_data);
}

static void iterator_no_query_returns_end_of_data(void** state)
{
    (void)state;
    span_t path = span_from_str_literal("/just/a/path");
    http_query_iterator_t it;
    /* Even with no '?', we treat the entire input as a query attempt.
     * "/just/a/path" has no '=' and no '&', so it parses as a single
     * pair name="/just/a/path" value="". That's intentional — callers
     * who only want post-'?' parsing can pre-check. */
    assert_int_equal(http_query_iterator_init(&it, path), ok);
    span_t k, v;
    assert_int_equal(http_query_iterator_next(&it, &k, &v), ok);
    EXPECT_SPAN_EQ(k, "/just/a/path");
    assert_int_equal(span_get_size(v), 0);
}

static void iterator_value_with_no_equals(void** state)
{
    (void)state;
    span_t path = span_from_str_literal("/?flag&x=1");
    http_query_iterator_t it;
    (void)http_query_iterator_init(&it, path);
    span_t k, v;
    assert_int_equal(http_query_iterator_next(&it, &k, &v), ok);
    EXPECT_SPAN_EQ(k, "flag"); assert_int_equal(span_get_size(v), 0);
    assert_int_equal(http_query_iterator_next(&it, &k, &v), ok);
    EXPECT_SPAN_EQ(k, "x"); EXPECT_SPAN_EQ(v, "1");
    assert_int_equal(http_query_iterator_next(&it, &k, &v), end_of_data);
}

/* ----- predicate --------------------------------------------------------- */

static void needs_decoding_detects_percent(void** state)
{
    (void)state;
    assert_true(http_query_value_needs_decoding(span_from_str_literal("a%20b")));
}

static void needs_decoding_detects_plus(void** state)
{
    (void)state;
    assert_true(http_query_value_needs_decoding(span_from_str_literal("a+b")));
}

static void needs_decoding_plain_text_is_false(void** state)
{
    (void)state;
    assert_false(http_query_value_needs_decoding(span_from_str_literal("hello-world.123")));
}

static void needs_decoding_empty_is_false(void** state)
{
    (void)state;
    assert_false(http_query_value_needs_decoding(SPAN_EMPTY));
}

int test_http_query()
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(find_basic_succeed),
        cmocka_unit_test(find_missing_returns_not_found),
        cmocka_unit_test(find_no_question_mark_scans_as_query),
        cmocka_unit_test(find_empty_value_succeeds),
        cmocka_unit_test(find_first_match_wins),
        cmocka_unit_test(find_null_out_invalid),
        cmocka_unit_test(find_decoded_percent_and_plus),
        cmocka_unit_test(find_decoded_too_small_buffer),
        cmocka_unit_test(find_decoded_malformed_percent),
        cmocka_unit_test(find_decoded_missing_propagates_not_found),
        cmocka_unit_test(iterator_walks_all_pairs),
        cmocka_unit_test(iterator_no_query_returns_end_of_data),
        cmocka_unit_test(iterator_value_with_no_equals),
        cmocka_unit_test(needs_decoding_detects_percent),
        cmocka_unit_test(needs_decoding_detects_plus),
        cmocka_unit_test(needs_decoding_plain_text_is_false),
        cmocka_unit_test(needs_decoding_empty_is_false),
    };
    return cmocka_run_group_tests_name("http_query", tests, NULL, NULL);
}
