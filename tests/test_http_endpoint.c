#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <inttypes.h>

#include <cmocka.h>

#include "niceties.h"

#include <http_request.h>
#include "http_endpoint.h"
#include "http_methods.h"
#include "http_versions.h"
#include "http_codes.h"

#include <test_http.h>

#define CLIENT_CERT_PATH "/home/ewertons/code/s1/http_listener/tests/scripts/certs/client.cert.pem"
#define CLIENT_PK_PATH "/home/ewertons/code/s1/http_listener/tests/scripts/private/client.key.pem"
#define SERVER_CERT_PATH "/home/ewertons/code/s1/http_listener/tests/scripts/certs/server.cert.pem"
#define SERVER_PK_PATH "/home/ewertons/code/s1/http_listener/tests/scripts/private/server.key.pem"
#define CA_CHAIN_PATH "/home/ewertons/code/s1/http_listener/tests/scripts/certs/chain.ca.cert.pem"

static uint8_t TEST_HTTP_REQUEST_GET_1[] = "GET / HTTP/1.1\r\n\
Host: localhost:1234\r\n\
User-Agent: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:75.0) Gecko/20100101 Firefox/75.0\r\n\
Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8\r\n\
Accept-Language: en-US,en;q=0.5\r\n\
Accept-Encoding: gzip, deflate, br\r\n\
Connection: keep-alive\r\n\
Upgrade-Insecure-Requests: 1\r\n\
\r\n";

static uint8_t TEST_HTTP_RESPONSE_GET_1[] = "HTTP/1.1 200 OK\r\n\
Server: http-c\r\n\
Content-Type: text/html; charset=UTF-8\r\n\
Content-Length: 43\r\n\
\r\n";

static uint8_t test_raw_buffer[1024];

static span_t HTTP_HEADER_USER_AGENT_VALUE = span_from_str_literal("Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:75.0) Gecko/20100101 Firefox/75.0");
static span_t HTTP_HEADER_CONNECTION_VALUE = span_from_str_literal("keep-alive");

static span_t HTTP_HEADER_SERVER_VALUE = span_from_str_literal("http-c");
static span_t HTTP_HEADER_CONTENT_TYPE_VALUE = span_from_str_literal("text/html; charset=UTF-8");
static span_t HTTP_HEADER_CONTENT_LENGTH_VALUE = span_from_str_literal("43");

static void http_endpoint_init_listener_succeed(void** state)
{
    (void)state;
    http_endpoint_t local_endpoint;
    http_endpoint_config_t local_endpoint_config = http_endpoint_get_default_secure_server_config();
    local_endpoint_config.local.port = 4343;

    assert_int_equal(http_endpoint_init(&local_endpoint, &local_endpoint_config), ok);
    assert_int_equal(http_endpoint_deinit(&local_endpoint), ok);
}

static void http_endpoint_client_and_server_succeed(void** state)
{
    (void)state;
    http_endpoint_t server_endpoint;
    http_endpoint_config_t server_endpoint_config = http_endpoint_get_default_secure_server_config();
    server_endpoint_config.local.port = 4344;
    server_endpoint_config.tls.certificate_file = SERVER_CERT_PATH;
    server_endpoint_config.tls.private_key_file = SERVER_PK_PATH;

    http_endpoint_t client_endpoint;
    http_endpoint_config_t client_endpoint_config = http_endpoint_get_default_secure_client_config();
    client_endpoint_config.remote.hostname = span_from_str_literal("localhost");
    client_endpoint_config.remote.port = server_endpoint_config.local.port;
    client_endpoint_config.tls.certificate_file = CLIENT_CERT_PATH;
    client_endpoint_config.tls.private_key_file = CLIENT_PK_PATH;
    client_endpoint_config.tls.trusted_certificate_file = CA_CHAIN_PATH;

    assert_int_equal(http_endpoint_init(&server_endpoint, &server_endpoint_config), ok);

    assert_int_equal(http_endpoint_init(&client_endpoint, &client_endpoint_config), ok);

    http_connection_t server_connection;
    task_t* wait_for_connection_task = http_endpoint_wait_for_connection_async(&server_endpoint, &server_connection);

    http_connection_t client_connection;
    assert_int_equal(http_endpoint_connect(&client_endpoint, &client_connection), ok);

    assert_true(task_wait(wait_for_connection_task));
    // assert_int_equal(, completed_successfully);

    span_t test_buffer = span_from_memory(test_raw_buffer);

    http_headers_t outgoing_request_headers, outgoing_response_headers;
    http_request_t outgoing_request;
    http_request_t incoming_request;
    http_response_t outgoing_response;
    http_response_t incoming_response;
    span_t header_name, header_value;

    assert_int_equal(http_headers_init(&outgoing_request_headers, test_buffer), HL_RESULT_OK);
    assert_int_equal(http_headers_add(&outgoing_request_headers, HTTP_HEADER_USER_AGENT, HTTP_HEADER_USER_AGENT_VALUE), HL_RESULT_OK);
    assert_int_equal(http_headers_add(&outgoing_request_headers, HTTP_HEADER_CONNECTION, HTTP_HEADER_CONNECTION_VALUE), HL_RESULT_OK);
    assert_int_equal(http_request_initialize(&outgoing_request, HTTP_METHOD_GET, span_from_str_literal("/"), HTTP_VERSION_1_1, outgoing_request_headers), ok);
    assert_int_equal(http_connection_send_request(&client_connection, &outgoing_request), ok);

    assert_int_equal(http_connection_receive_request(&server_connection, test_buffer, &incoming_request, NULL), ok);
    assert_int_equal(span_compare(incoming_request.method, HTTP_METHOD_GET), 0);
    assert_int_equal(span_compare(incoming_request.path, span_from_str_literal("/")), 0);
    assert_int_equal(span_compare(incoming_request.http_version, HTTP_VERSION_1_1), 0);

    assert_int_equal(http_headers_get_next(&incoming_request.headers, &header_name, &header_value), HL_RESULT_OK);
    assert_int_equal(span_get_size(header_name), span_get_size(HTTP_HEADER_USER_AGENT)); 
    assert_memory_equal(span_get_ptr(header_name), span_get_ptr(HTTP_HEADER_USER_AGENT), span_get_size(HTTP_HEADER_USER_AGENT)); 
    assert_int_equal(span_get_size(header_value), span_get_size(HTTP_HEADER_USER_AGENT_VALUE)); 
    assert_memory_equal(span_get_ptr(header_value), span_get_ptr(HTTP_HEADER_USER_AGENT_VALUE), span_get_size(HTTP_HEADER_USER_AGENT_VALUE)); 
    assert_int_equal(http_headers_get_next(&incoming_request.headers, &header_name, &header_value), HL_RESULT_OK);
    assert_int_equal(span_get_size(header_name), span_get_size(HTTP_HEADER_CONNECTION)); 
    assert_memory_equal(span_get_ptr(header_name), span_get_ptr(HTTP_HEADER_CONNECTION), span_get_size(HTTP_HEADER_CONNECTION)); 
    assert_int_equal(span_get_size(header_value), span_get_size(HTTP_HEADER_CONNECTION_VALUE)); 
    assert_memory_equal(span_get_ptr(header_value), span_get_ptr(HTTP_HEADER_CONNECTION_VALUE), span_get_size(HTTP_HEADER_CONNECTION_VALUE));
    assert_int_equal(http_headers_get_next(&incoming_request.headers, &header_name, &header_value), HL_RESULT_EOF);

    assert_int_equal(http_headers_init(&outgoing_response_headers, test_buffer), HL_RESULT_OK);
    assert_int_equal(http_headers_add(&outgoing_response_headers, HTTP_HEADER_SERVER, HTTP_HEADER_SERVER_VALUE), HL_RESULT_OK);
    assert_int_equal(http_headers_add(&outgoing_response_headers, HTTP_HEADER_CONTENT_TYPE, HTTP_HEADER_CONTENT_TYPE_VALUE), HL_RESULT_OK);
    assert_int_equal(http_headers_add(&outgoing_response_headers, HTTP_HEADER_CONTENT_LENGTH, HTTP_HEADER_CONTENT_LENGTH_VALUE), HL_RESULT_OK);
    assert_int_equal(http_response_initialize(&outgoing_response, HTTP_VERSION_1_1, HTTP_CODE_200, HTTP_REASON_PHRASE_200, outgoing_response_headers), ok);
    assert_int_equal(http_connection_send_response(&server_connection, &outgoing_response), ok);

    assert_int_equal(http_connection_receive_response(&client_connection, test_buffer, &incoming_response, NULL), ok);
    assert_int_equal(span_compare(incoming_response.http_version, HTTP_VERSION_1_1), 0);
    assert_int_equal(span_compare(incoming_response.code, HTTP_CODE_200), 0);
    assert_int_equal(span_compare(incoming_response.reason_phrase, HTTP_REASON_PHRASE_200), 0);

    assert_int_equal(http_headers_get_next(&incoming_response.headers, &header_name, &header_value), HL_RESULT_OK);
    assert_int_equal(span_compare(header_name, HTTP_HEADER_SERVER), 0);
    assert_int_equal(span_compare(header_value, HTTP_HEADER_SERVER_VALUE), 0);
    assert_int_equal(http_headers_get_next(&incoming_response.headers, &header_name, &header_value), HL_RESULT_OK);
    assert_int_equal(span_compare(header_name, HTTP_HEADER_CONTENT_TYPE), 0);
    assert_int_equal(span_compare(header_value, HTTP_HEADER_CONTENT_TYPE_VALUE), 0);
    assert_int_equal(http_headers_get_next(&incoming_response.headers, &header_name, &header_value), HL_RESULT_OK);
    assert_int_equal(span_compare(header_name, HTTP_HEADER_CONTENT_LENGTH), 0);
    assert_int_equal(span_compare(header_value, HTTP_HEADER_CONTENT_LENGTH_VALUE), 0);
    assert_int_equal(http_headers_get_next(&incoming_response.headers, &header_name, &header_value), HL_RESULT_EOF);

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
