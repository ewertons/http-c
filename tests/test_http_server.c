#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <inttypes.h>

#include <cmocka.h>

#include "niceties.h"

#include <http_request.h>
#include <http_response.h>
#include "http_methods.h"
#include "http_server.h"

#include <test_http.h>

#define CLIENT_CERT_PATH "TBD"
#define CLIENT_PK_PATH "TBD"
#define SERVER_CERT_PATH "TBD"
#define SERVER_PK_PATH "TBD"
#define CA_CHAIN_PATH "TBD"

static uint8_t TEST_HTTP_REQUEST_GET_1[] = "GET / HTTP/1.1\r\n\
Host: localhost:1234\r\n\
User-Agent: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:75.0) Gecko/20100101 Firefox/75.0\r\n\
Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8\r\n\
Accept-Language: en-US,en;q=0.5\r\n\
Accept-Encoding: gzip, deflate, br\r\n\
Connection: keep-alive\r\n\
Upgrade-Insecure-Requests: 1\r\n\
\r\n";

static void handle_http_request(http_request_t* request, span_t* path_matches, uint16_t number_of_matches, http_response_t* out_response, void* user_context)
{
    (void)request;
    (void)path_matches;
    (void)number_of_matches;
    (void)out_response;
    (void)user_context;
}

static void http_server_run_succeed(void** state)
{
    (void)state;
    http_server_t http_server;

    http_server_config_t http_server_config = http_server_get_default_config();
    http_server_config.tls.certificate_file = SERVER_CERT_PATH;
    http_server_config.tls.private_key_file = SERVER_PK_PATH;

    assert_int_equal(http_server_init(&http_server, &http_server_config), ok);

    assert_int_equal(http_server_add_route(&http_server, HTTP_METHOD_POST, span_from_str_literal("/cars/*/*/*"), handle_http_request, NULL), ok);
    assert_int_equal(http_server_add_route(&http_server, HTTP_METHOD_GET, span_from_str_literal("/index.html"), handle_http_request, NULL), ok);
    assert_int_equal(http_server_add_route(&http_server, HTTP_METHOD_GET, span_from_str_literal("/"), handle_http_request, NULL), ok);

    task_t* http_server_task = http_server_run_async(&http_server);
    assert_non_null(http_server_task);


    task_cancel(http_server_task);
    assert_true(task_wait(http_server_task));
}


int test_http_server()
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(http_server_run_succeed)
  };

  return cmocka_run_group_tests_name("http_server", tests, NULL, NULL);
}
