/*
 * shared_link_consumer.c
 *
 * Minimal program that pulls in the http-c (and common-lib-c) public
 * headers and calls a small handful of public API functions. The point
 * is NOT to exercise networking -- it is to prove that:
 *
 *   1. the public headers are self-contained (compile without dragging
 *      in OpenSSL or other backend headers),
 *   2. the symbols referenced below are exported by libhttp.so /
 *      libcommon-lib-c.so, and
 *   3. the binary runs successfully when the libraries are loaded as
 *      .so files via the dynamic linker (validated by simply executing
 *      it as part of ctest).
 *
 * When the parent CMakeLists is configured with -DBUILD_SHARED_LIBS=ON,
 * `http` and `common-lib-c` become shared objects and this binary is
 * the smoke test that they actually work.
 *
 * Returns 0 on success, non-zero on any unexpected failure.
 */

#include <stdio.h>
#include <string.h>

#include "common_lib_c.h"

#include "http_server.h"
#include "http_endpoint.h"
#include "http_methods.h"
#include "http_codes.h"
#include "http_versions.h"
#include "http_headers.h"
#include "http_server_storage.h"
#include "socket.h"

static int check(bool cond, const char* msg)
{
    if (!cond)
    {
        fprintf(stderr, "[shared_link_consumer] FAIL: %s\n", msg);
        return 1;
    }
    return 0;
}

int main(void)
{
    int rc = 0;

    /* -- defaults helpers (inline functions in headers) -- */
    http_server_config_t scfg = http_server_get_default_config();
    rc |= check(scfg.tls.enable == true,
                "http_server_get_default_config: tls.enable should default to true");

    http_endpoint_config_t ecfg_secure =
        http_endpoint_get_default_secure_client_config();
    rc |= check(ecfg_secure.tls.enable == true,
                "secure client config should have tls.enable=true");

    socket_config_t plain_srv = socket_get_default_plain_server_config();
    rc |= check(plain_srv.tls.enable == false,
                "plain server config should have tls.enable=false");

    /* -- exercise actual exported library functions, not just inlines -- */
    http_server_t server;
    http_server_config_t cfg = http_server_get_default_config();
    cfg.tls.enable = false;          /* plain HTTP, no certs needed */
    cfg.port       = 0;              /* never actually bound */

    if (http_server_init(&server, &cfg,
                         http_server_storage_get_for_server_host()) != ok)
    {
        fprintf(stderr, "[shared_link_consumer] FAIL: http_server_init\n");
        return 2;
    }

    if (http_server_add_route(&server, HTTP_METHOD_GET,
                              span_from_str_literal("^/$"),
                              (http_request_handler_t)0x1, /* unused; not invoked */
                              NULL) != ok)
    {
        fprintf(stderr, "[shared_link_consumer] FAIL: http_server_add_route\n");
        (void)http_server_deinit(&server);
        return 3;
    }

    if (http_server_deinit(&server) != ok)
    {
        fprintf(stderr, "[shared_link_consumer] FAIL: http_server_deinit\n");
        return 4;
    }

    /* -- common-lib-c symbol: span_compare from libcommon-lib-c.so -- */
    span_t a = span_from_str_literal("hello");
    span_t b = span_from_str_literal("hello");
    rc |= check(span_compare(a, b) == 0, "span_compare equal spans");

    if (rc == 0)
    {
        printf("[shared_link_consumer] OK\n");
    }
    return rc;
}
