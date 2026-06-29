#pragma once
#include "esp_err.h"
/* STA bring-up; blocks up to timeout for first IP, retries in background. */
esp_err_t bench_wifi_connect(void);
