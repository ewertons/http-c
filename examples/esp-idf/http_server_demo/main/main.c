/*
 * http-c ESP-IDF example: a plain-HTTP server on port 80.
 *
 * The device joins a network using the standard ESP-IDF
 * `protocol_examples_common` helper (configure SSID/password or Ethernet
 * via `idf.py menuconfig` -> "Example Connection Configuration"), then
 * starts an http-c server that answers `GET /` with a small HTML page.
 *
 * TLS is disabled on this target (ESP-IDF ships mbedTLS, not OpenSSL, so the
 * http-c component is built with SOCKET_TLS_NONE). The portable select()
 * event-loop backend is used because lwIP has no epoll/eventfd.
 */

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

#include "span.h"
#include "http_codes.h"
#include "http_headers.h"
#include "http_methods.h"
#include "http_response.h"
#include "http_request.h"
#include "http_server.h"
#include "http_server_storage.h"

static const char* TAG = "http_c_demo";

/* Listen on the standard HTTP port. The library default is 443 (HTTPS); we
 * override it because TLS is disabled on this build. */
#define DEMO_SERVER_PORT 80

static const span_t INDEX_BODY = span_from_str_literal(
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>http-c on ESP32</title></head>"
    "<body><h1>Hello from http-c</h1>"
    "<p>Served by the http-c library running on ESP-IDF.</p>"
    "</body></html>\n");
static const span_t CONTENT_TYPE_HTML = span_from_str_literal("text/html");

static uint8_t s_response_headers_buffer[128];

static void index_handler(http_request_t*  request,
                          span_t*          path_matches,
                          uint16_t         path_match_count,
                          http_response_t* out_response,
                          void*            user_context)
{
    (void)request;
    (void)path_matches;
    (void)path_match_count;
    (void)user_context;

    if (http_headers_init(&out_response->headers,
                          span_init(s_response_headers_buffer,
                                    (uint32_t)sizeof(s_response_headers_buffer))) != HL_RESULT_OK)
    {
        return;
    }

    static char content_length_buffer[12];
    int content_length_len = snprintf(content_length_buffer,
                                      sizeof(content_length_buffer),
                                      "%u",
                                      (unsigned)span_get_size(INDEX_BODY));

    (void)http_headers_add(&out_response->headers,
                           HTTP_HEADER_CONTENT_TYPE, CONTENT_TYPE_HTML);
    (void)http_headers_add(&out_response->headers,
                           HTTP_HEADER_CONTENT_LENGTH,
                           span_init((uint8_t*)content_length_buffer,
                                     (uint32_t)content_length_len));

    out_response->code          = HTTP_CODE_200;
    out_response->reason_phrase = HTTP_REASON_PHRASE_200;
    out_response->body          = INDEX_BODY;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Bring up Wi-Fi or Ethernet using the project's connection settings. */
    ESP_ERROR_CHECK(example_connect());

    http_server_t server;
    http_server_config_t cfg = { 0 };
    cfg.port           = DEMO_SERVER_PORT;
    cfg.tls.enable     = false; /* plain HTTP: no OpenSSL on ESP-IDF */

    if (http_server_init(&server, &cfg,
                         http_server_storage_get_for_microcontroller()) != ok)
    {
        ESP_LOGE(TAG, "http_server_init failed");
        return;
    }

    if (http_server_add_route(&server, HTTP_METHOD_GET,
                              span_from_str_literal("^/$"),
                              index_handler, NULL) != ok)
    {
        ESP_LOGE(TAG, "http_server_add_route failed");
        (void)http_server_deinit(&server);
        return;
    }

    ESP_LOGI(TAG, "http-c server listening on port %d", DEMO_SERVER_PORT);

    /* Runs the event loop on this task until http_server_stop is called. */
    (void)http_server_run(&server);
    (void)http_server_deinit(&server);
}
