/*
 * Benchmark firmware A: the http-c library serving GET / over plain HTTP.
 *
 * Routes (identical to the esp_http_server peer firmware):
 *   GET /       -> "hello\n"  (text/plain, Content-Length: 6)
 *   GET /stats  -> JSON: free/min heap, uptime, request count, idle counters
 *
 * TLS is disabled (ESP-IDF ships mbedTLS, not OpenSSL). Port 80.
 */

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "bench_wifi.h"

#include "span.h"
#include "http_codes.h"
#include "http_headers.h"
#include "http_methods.h"
#include "http_response.h"
#include "http_request.h"
#include "http_server.h"
#include "http_server_storage.h"

static const char* TAG = "bench_httpc";
#define DEMO_SERVER_PORT 80

static const span_t HELLO_BODY        = span_from_str_literal("hello\n");
static const span_t CONTENT_TYPE_TEXT = span_from_str_literal("text/plain");

static volatile uint32_t s_req_count = 0;

static uint8_t s_hdr_buf[128];
static char    s_json_buf[192];

static void index_handler(http_request_t* request, span_t* m, uint16_t n,
                          http_response_t* out, void* ctx)
{
    (void)request; (void)m; (void)n; (void)ctx;
    s_req_count++;
    if (http_headers_init(&out->headers, span_init(s_hdr_buf, sizeof(s_hdr_buf))) != HL_RESULT_OK)
        return;
    (void)http_headers_add(&out->headers, HTTP_HEADER_CONTENT_TYPE, CONTENT_TYPE_TEXT);
    (void)http_headers_add(&out->headers, HTTP_HEADER_CONTENT_LENGTH, span_from_str_literal("6"));
    out->code = HTTP_CODE_200; out->reason_phrase = HTTP_REASON_PHRASE_200; out->body = HELLO_BODY;
}

static void stats_handler(http_request_t* request, span_t* m, uint16_t n,
                          http_response_t* out, void* ctx)
{
    (void)request; (void)m; (void)n; (void)ctx;
    int len = snprintf(s_json_buf, sizeof(s_json_buf),
        "{\"server\":\"http-c\",\"free_heap\":%u,\"min_free_heap\":%u,"
        "\"uptime_ms\":%llu,\"requests\":%u}\n",
        (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned long long)(esp_timer_get_time()/1000), (unsigned)s_req_count);
    static char cl[8]; int cll = snprintf(cl, sizeof(cl), "%d", len);
    if (http_headers_init(&out->headers, span_init(s_hdr_buf, sizeof(s_hdr_buf))) != HL_RESULT_OK)
        return;
    (void)http_headers_add(&out->headers, HTTP_HEADER_CONTENT_TYPE, span_from_str_literal("application/json"));
    (void)http_headers_add(&out->headers, HTTP_HEADER_CONTENT_LENGTH, span_init((uint8_t*)cl, cll));
    out->code = HTTP_CODE_200; out->reason_phrase = HTTP_REASON_PHRASE_200;
    out->body = span_init((uint8_t*)s_json_buf, len);
}

void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(bench_wifi_connect());

    static http_server_t server;   /* ~20KB (event_loop table): keep off the stack */
    http_server_config_t cfg = { 0 };
    cfg.port = DEMO_SERVER_PORT;
    cfg.tls.enable = false;
    if (http_server_init(&server, &cfg, http_server_storage_get_for_microcontroller()) != ok) {
        ESP_LOGE(TAG, "http_server_init failed"); return;
    }
    (void)http_server_add_route(&server, HTTP_METHOD_GET, span_from_str_literal("^/$"), index_handler, NULL);
    (void)http_server_add_route(&server, HTTP_METHOD_GET, span_from_str_literal("^/stats$"), stats_handler, NULL);
    ESP_LOGI(TAG, "http-c bench server on port %d", DEMO_SERVER_PORT);
    (void)http_server_run(&server);
    (void)http_server_deinit(&server);
}
