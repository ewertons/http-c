/*
 * Benchmark firmware B: ESP-IDF's built-in esp_http_server serving GET /.
 *
 * Routes (identical to the http-c peer firmware):
 *   GET /       -> "hello\n"  (text/plain, Content-Length: 6)
 *   GET /stats  -> JSON: free/min heap, uptime, request count
 *
 * max_open_sockets is pinned to 4 to match http-c's microcontroller storage
 * preset (HTTP_SERVER_MCU_CONNECTIONS = 4) so capacity is comparable. Port 80.
 */

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "bench_wifi.h"

static const char* TAG = "bench_esp";
static volatile uint32_t s_req_count = 0;

static esp_err_t index_handler(httpd_req_t* req)
{
    s_req_count++;
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "hello\n", 6);
}

static esp_err_t stats_handler(httpd_req_t* req)
{
    char buf[192];
    int len = snprintf(buf, sizeof(buf),
        "{\"server\":\"esp_http_server\",\"free_heap\":%u,\"min_free_heap\":%u,"
        "\"uptime_ms\":%llu,\"requests\":%u}\n",
        (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned long long)(esp_timer_get_time()/1000), (unsigned)s_req_count);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
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

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_open_sockets  = 4;       /* match http-c MCU preset */
    cfg.lru_purge_enable  = true;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd_start failed"); return; }

    httpd_uri_t root = { .uri="/", .method=HTTP_GET, .handler=index_handler };
    httpd_uri_t stats = { .uri="/stats", .method=HTTP_GET, .handler=stats_handler };
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &stats);
    ESP_LOGI(TAG, "esp_http_server bench on port 80");
}
