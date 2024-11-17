#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <inttypes.h>

#include <cmocka.h>

#include "niceties.h"

#include <http_request.h>
#include "http_endpoint.h"

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


// static int internal_do(void* args)
// {
//   task_t res = another_func_async();

//   task_await(res);

//   return 0;
// }

// task_t do_async(int a)
// {
//   struct custom_args args;
//   args.a = a;

//   return task_run(&args, internal_do);
// }


static void http_endpoint_init_listener_succeed(void** state)
{
    (void)state;
    http_endpoint_t local_endpoint;
    http_endpoint_config_t local_endpoint_config;
    local_endpoint_config.port = 4344;

    assert_int_equal(http_endpoint_init(&local_endpoint, &local_endpoint_config), ok);
}

static void http_endpoint_client_and_server_succeed(void** state)
{
    (void)state;
    http_endpoint_t server_endpoint;
    http_endpoint_config_t server_endpoint_config;
    server_endpoint_config.port = 4344;
    server_endpoint_config.tls.enable = true;

    http_endpoint_t client_endpoint;
    http_endpoint_config_t client_endpoint_config;
    client_endpoint_config.hostname = span_from_str_literal("localhost");
    client_endpoint_config.port = 4344;
    client_endpoint_config.tls.enable = true;

    assert_int_equal(http_endpoint_init(&server_endpoint, &server_endpoint_config), ok);

    assert_int_equal(http_endpoint_init(&client_endpoint, &client_endpoint_config), ok);

    http_connection_t server_connection;
    task_t* wait_for_connection_task = http_endpoint_wait_for_connection_async(&server_endpoint, &server_connection);

    http_connection_t client_connection;
    assert_int_equal(http_endpoint_connect(&client_endpoint, &client_connection), ok);

    assert_int_equal(task_await(wait_for_connection_task), completed_successfully);

    http_request_t outgoing_request;
    http_request_t incoming_request;
    http_response_t outgoing_response;
    http_response_t incoming_response;

    assert_int_equal(http_connection_send_request(&client_connection, &outgoing_request), ok);
    assert_int_equal(http_connection_receive_request(&server_connection, &incoming_request), ok);
    assert_int_equal(http_connection_send_response(&server_connection, &outgoing_response), ok);
    assert_int_equal(http_connection_receive_response(&client_connection, &incoming_response), ok);

    assert_int_equal(http_connection_close(&client_connection), ok);
    assert_int_equal(http_connection_close(&server_connection), ok);
    assert_int_equal(http_endpoint_deinit(&client_endpoint), ok);
    assert_int_equal(http_endpoint_deinit(&server_endpoint), ok);
}

int test_http_endpoint()
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(http_endpoint_init_listener_succeed),
    cmocka_unit_test(http_endpoint_client_and_server_succeed)
  };

  return cmocka_run_group_tests_name("http_endpoint", tests, NULL, NULL);
}
