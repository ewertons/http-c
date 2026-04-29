#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>

#include "http_server.h"
#include "http_server_storage.h"

#include <test_http.h>

static void server_host_storage_returns_non_null_with_buffers(void** state)
{
    (void)state;

    http_server_storage_t* storage = http_server_storage_get_for_server_host();
    assert_non_null(storage);
    assert_non_null(storage->slots);
    assert_non_null(storage->routes);
    assert_true(storage->slot_count > 0);
    assert_true(storage->route_count > 0);

    for (size_t i = 0; i < storage->slot_count; i++)
    {
        assert_non_null(storage->slots[i].recv_buffer_ptr);
        assert_true(storage->slots[i].recv_buffer_size > 0);
        assert_non_null(storage->slots[i].send_buffer_ptr);
        assert_true(storage->slots[i].send_buffer_size > 0);
    }
}

static void server_host_storage_is_singleton(void** state)
{
    (void)state;

    http_server_storage_t* a = http_server_storage_get_for_server_host();
    http_server_storage_t* b = http_server_storage_get_for_server_host();

    assert_ptr_equal(a, b);
}

static void mcu_storage_returns_non_null_with_buffers(void** state)
{
    (void)state;

    http_server_storage_t* storage = http_server_storage_get_for_microcontroller();
    assert_non_null(storage);
    assert_non_null(storage->slots);
    assert_non_null(storage->routes);
    assert_true(storage->slot_count > 0);
    assert_true(storage->route_count > 0);

    for (size_t i = 0; i < storage->slot_count; i++)
    {
        assert_non_null(storage->slots[i].recv_buffer_ptr);
        assert_true(storage->slots[i].recv_buffer_size > 0);
        assert_non_null(storage->slots[i].send_buffer_ptr);
        assert_true(storage->slots[i].send_buffer_size > 0);
    }
}

static void mcu_storage_is_singleton(void** state)
{
    (void)state;

    http_server_storage_t* a = http_server_storage_get_for_microcontroller();
    http_server_storage_t* b = http_server_storage_get_for_microcontroller();

    assert_ptr_equal(a, b);
}

static void server_host_and_mcu_storage_are_distinct(void** state)
{
    (void)state;

    http_server_storage_t* host = http_server_storage_get_for_server_host();
    http_server_storage_t* mcu = http_server_storage_get_for_microcontroller();

    assert_ptr_not_equal(host, mcu);
}

int test_http_server_storage(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(server_host_storage_returns_non_null_with_buffers),
        cmocka_unit_test(server_host_storage_is_singleton),
        cmocka_unit_test(mcu_storage_returns_non_null_with_buffers),
        cmocka_unit_test(mcu_storage_is_singleton),
        cmocka_unit_test(server_host_and_mcu_storage_are_distinct),
    };

    return cmocka_run_group_tests_name("http_server_storage", tests, NULL, NULL);
}
