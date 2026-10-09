#ifndef TEST_HTTPS_LISTENER_H
#define TEST_HTTPS_LISTENER_H

int test_http_request();
int test_http_request_parser();
int test_http_headers();
int test_http_query();
int test_http_response();
int test_http_server();
int test_http_endpoint();
int test_http_server_storage();

/* Where every TLS test reads its fixtures, minted by scripts/run-coverage.sh. */
#define TEST_CLIENT_CERT_PATH "/tmp/http-c-certs/client/client.cert.pem"
#define TEST_SERVER_CERT_PATH "/tmp/http-c-certs/server/server.cert.pem"
#define TEST_CA_CHAIN_PATH    "/tmp/http-c-certs/ca/chain.ca.cert.pem"

/* Fails the calling test, by name, when a fixture is missing or expired. */
void assert_tls_fixtures_current(void);

#endif // TEST_HTTPS_LISTENER_H