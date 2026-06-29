# Copilot notes for the http-c repo

Persistent, verified context to help future work in this repository.

## Repo layout
- Library: `components/http-c` (+ `deps/common-lib-c/components`).
- Desktop benchmark: `benchmarks/REPORT.md` (http-c vs mongoose, HTTPS loopback).
- ESP32 on-device benchmark: `benchmarks/esp32/` (http-c vs esp_http_server).
- The ESP32 http-c firmware references the library via the repo root:
  in `benchmarks/esp32/firmware-httpc/CMakeLists.txt`,
  `HTTP_C_REPO_ROOT = ${CMAKE_CURRENT_LIST_DIR}/../../..`.

## ESP-IDF v6 compatibility (confirmed root causes)
1. **Stack overflow — not a heap/pthread bug.** `http_server_t` embeds an
   event-loop descriptor table `table[EVENT_LOOP_MAX_FDS]` (default 1024 ×
   20 B ≈ 20 KB). Declaring `http_server_t server;` as a stack local in
   `app_main` overflows the 12 KB main task stack and corrupts the heap,
   which then faults the *first* `nvs_flash_init()` malloc. The crash shows
   as `LoadProhibited` inside the heap-lock spinlock — a misleading symptom.
   Fix: make the server instance `static` (off the stack) **and** set
   `CONFIG_ESP_MAIN_TASK_STACK_SIZE=32768`.
   (An earlier "uninitialized pthread mutex / `task_platform_init` ordering"
   theory was investigated and found WRONG.)
2. **Wi-Fi bring-up.** IDF v6 `protocol_examples_common` `example_connect()`
   aborts inside `esp_wifi_init` on the v6 toolchain. Fix: a small
   self-contained `bench_wifi.c` (STA connect + auto-retry, blocks for the
   first IP, reuses `CONFIG_EXAMPLE_WIFI_SSID/PASSWORD`). Component
   `REQUIRES esp_wifi` instead of `protocol_examples_common`.

These fixes are configuration-level and keep the firmware building on both
ESP-IDF v5.4 and v6.

## ESP32 benchmark headline (ESP32 @240 MHz, plain HTTP, 4-conn cap, GET / = 6 B)
- http-c best 122.7 req/s @4 conns; esp_http_server 58.5 @8 → http-c ≈ 2.1×.
- Est. CPU: http-c 16.3k vs esp 34.2k µs/req. Heap under load: 22.2 vs 30.5 KB.
- esp keeps more absolute free heap (http-c statically embeds its tables).
- App binary ≈ 803 KB (http-c) / 805 KB (esp).

## Security / hygiene (ESP-IDF projects)
- NEVER commit `sdkconfig` (contains the live Wi-Fi password in plaintext),
  `build/`, or `managed_components/` (auto-downloaded). Only commit
  `sdkconfig.defaults` (SSID only, no password). All three are gitignored in
  the firmware directories.

## Workflow tips
- Only ESP-IDF v6.0.1 is installed here (`. ~/esp/esp-idf/export.sh`); target
  `esp32`, serial `/dev/ttyUSB0`.
- `idf.py monitor` piped to a file truncates on SIGTERM/timeout and loses late
  lines. To capture boot reliably, read the serial port directly with pyserial
  (toggle RTS to reset, then loop-read `in_waiting` for N seconds into a file).
