#ifndef HTTP_SERVER_STORAGE_H
#define HTTP_SERVER_STORAGE_H

#include "http_server.h"

/* Two ready-to-use storage providers, each backed by static arrays sized
 * by compile-time tunables. They live in separate translation units so a
 * project that only uses one preset doesn't pay for the other in BSS.
 *
 * Override the sizes from CMake / your build system if the defaults don't
 * fit your target, e.g.:
 *     -DHTTP_SERVER_HOST_CONNECTIONS=512
 *     -DHTTP_SERVER_MCU_REQUEST_BUFFER_SIZE=2048
 */

/**
 * @brief Server-class preset (Linux / BSD hosts, server-side use).
 *
 * Defaults:
 *   HTTP_SERVER_HOST_CONNECTIONS         = 256
 *   HTTP_SERVER_HOST_ROUTES              = 32
 *   HTTP_SERVER_HOST_REQUEST_BUFFER_SIZE = 8192
 */
http_server_storage_t* http_server_storage_get_for_server_host(void);

/**
 * @brief Microcontroller / FreeRTOS preset.
 *
 * Defaults:
 *   HTTP_SERVER_MCU_CONNECTIONS         = 4
 *   HTTP_SERVER_MCU_ROUTES              = 8
 *   HTTP_SERVER_MCU_REQUEST_BUFFER_SIZE = 1024
 */
http_server_storage_t* http_server_storage_get_for_microcontroller(void);

#endif /* HTTP_SERVER_STORAGE_H */
