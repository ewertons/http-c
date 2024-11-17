#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <inttypes.h>

#include <cmocka.h>

#include "niceties.h"

#include <http_request.h>
#include "http_server.h"

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

static void http_server_init_succeed(void** state)
{
    (void)state;
    http_server_t http_server;

    http_server_config_t http_server_config;
    http_server_config.port = 10000;
    // 1 [GOOD]
    http_server_init(&http_server, &http_server_config);

    // http_server_add_route(&http, POST, "/cars/*/*/*", a);
    // http_server_add_route(&http, GET, "/index.html", a);
    // http_server_add_route(&http, GET, "/", a);
    // http_server_run(&http_server);
    // // 2
    // http_connection_t http_connection;
    // http_connection_start(&http_connection, &http_connection_config);
    // http_connection_send_request(&http_connection, &http_request);
    // http_connection_receive_response(&http_connection, &http_response);

    // http_connection_t http_connection;
    // http_connection_listen(&http_connection, &http_connection_config);
    // http_connection_receive_request(&http_connection, &http_response);
    // http_connection_send_response(&http_connection, &http_request);
    // // 3
    // http_t http;
    // http_connect(&http, &http_config);
    // http_send_request(&http, &http_request);
    // http_receive_response(&http, &http_response);

    // http_t http;
    // http_listen(&http, &http_config, &http_client);
    // http_receive_request(&http_client, &http_response);
    // http_send_response(&http_client, &http_request);

    // http_run_server();

    // 4


    
}

static void http_request_get_GET_succeed2(void** state)
{
    (void)state;

    http_request_t request;
    span_t method;
    span_t uri;
    span_t version;

    span_t buffer =  span_from_string(TEST_HTTP_REQUEST_GET_1);

    assert_int_equal(http_request_parse(&request, buffer), HL_RESULT_OK);
    assert_int_equal(http_request_get_method(&request, &method), HL_RESULT_OK);
    assert_int_equal(span_compare(method, span_from_str_literal("GET")), 0);

    assert_int_equal(http_request_get_path(&request, &uri), HL_RESULT_OK);
    assert_int_equal(span_compare(uri, span_from_str_literal("/")), 0);

    assert_int_equal(http_request_get_http_version(&request, &version), HL_RESULT_OK);
    assert_int_equal(span_compare(version, span_from_str_literal("1.1")), 0);
}

static void http_request_get_GET_headers_succeed2(void** state)
{
    (void)state;

    http_request_t request;
    http_headers_t headers;
    span_t name, value;

    span_t buffer =  span_from_string(TEST_HTTP_REQUEST_GET_1);

    assert_int_equal(http_request_parse(&request, buffer), HL_RESULT_OK);

    assert_int_equal(http_request_get_headers(&request, &headers), HL_RESULT_OK);

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

int test_http_server()
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(http_server_init_succeed)
  };

  return cmocka_run_group_tests_name("http_server", tests, NULL, NULL);
}