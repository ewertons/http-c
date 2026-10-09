#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>

#include <cmocka.h>

#include <test_http.h>

#if !defined(SOCKET_TLS_NONE)
#include <stdio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

/* The fixtures are minted for a limited time, and an expired one fails the
 * handshake exactly as a broken TLS path would -- which is how a month-old
 * set once read as a regression. Called before a test starts any thread, so
 * the failure is one clean line naming the cause. */
static void assert_tls_fixture_current(const char* path)
{
    FILE* file = fopen(path, "r");
    if (file == NULL)
    {
        fail_msg("TLS fixture %s is missing; mint the set with scripts/run-coverage.sh", path);
    }
    X509* certificate = PEM_read_X509(file, NULL, NULL, NULL);
    (void)fclose(file);
    if (certificate == NULL)
    {
        fail_msg("TLS fixture %s is not a PEM certificate", path);
    }
    bool expired = X509_cmp_current_time(X509_get0_notAfter(certificate)) <= 0;
    X509_free(certificate);
    if (expired)
    {
        fail_msg("TLS fixture %s has expired; re-mint the set with scripts/run-coverage.sh",
                 path);
    }
}

void assert_tls_fixtures_current(void)
{
    assert_tls_fixture_current(TEST_CA_CHAIN_PATH);
    assert_tls_fixture_current(TEST_SERVER_CERT_PATH);
    assert_tls_fixture_current(TEST_CLIENT_CERT_PATH);
}
#else
void assert_tls_fixtures_current(void)
{
}
#endif
