# http-c on ESP32 — embedded benchmark report

Head-to-head comparison of **http-c** against ESP-IDF's built-in
**esp_http_server** running on real ESP32 hardware over Wi-Fi.

This is the embedded counterpart to the Linux desktop benchmark in
[`../REPORT.md`](../REPORT.md) (http-c vs mongoose over loopback HTTPS).

## Test setup

- Workload: `GET /` returning `hello\n` (6 bytes) over plain HTTP/1.1
  with keep-alive. Both servers send an explicit `Content-Length: 6`.
- A second route, `GET /stats`, returns JSON with free/min-free heap,
  uptime and request count so heap can be sampled between sweep steps.
- Transport: plain HTTP on port 80. TLS is disabled — ESP-IDF ships
  mbedTLS rather than OpenSSL, so a like-for-like TLS path is out of
  scope for this run.
- Both firmwares pin the listener to **4 concurrent connections** so the
  capacity comparison is fair: http-c uses its microcontroller storage
  preset (`HTTP_SERVER_MCU_CONNECTIONS = 4`); esp_http_server sets
  `max_open_sockets = 4`.
- Load generator: [`loadgen.py`](loadgen.py) (async stdlib HTTP,
  keep-alive), driven by [`measure.py`](measure.py) across a connection
  sweep of 1, 2, 4, 8 and 16 connections, 15 s per step.
- Connection capacity / CPU-per-request are derived by
  [`measure.py`](measure.py); the comparison table is rendered by
  [`compare.py`](compare.py).

## Device under test

- SoC: ESP32 (chip rev v3.1), dual-core @ 240 MHz, 2 MB flash.
- ESP-IDF: `v6.0.1-706-ga0688c6e32`, target `esp32`.
- Network: 2.4 GHz Wi-Fi, station mode; load generator on the same LAN.

## Servers under test

| Server          | Source                              | I/O model |
|-----------------|-------------------------------------|-----------|
| http-c          | this repository (`components/http-c`) | single event loop (`select` backend), static caller-supplied storage, zero alloc per request |
| esp_http_server | ESP-IDF built-in component          | dedicated server task, per-connection scratch buffers |

## Results

| Metric                     | http-c   | esp_http_server | Delta |
|----------------------------|----------|-----------------|-------|
| Best req/s                 | **122.7**| 58.5            | **2.10× http-c** |
| Connections at best        | 4        | 8               | — |
| Est. CPU µs/req            | **16,300** | 34,188        | **2.10× less http-c** |
| Heap used under load (B)   | **22,184** | 30,508        | **8.3 KB less http-c** |
| Base free heap (B)         | 157,172  | 224,444         | esp +67 KB |
| Peak min-free heap (B)     | 134,988  | 193,936         | esp +59 KB |
| App binary (B)             | **802,896** | 805,488      | http-c −2.6 KB |
| p50 / p99 latency (4 conn) | **31.5 / 63.9 ms** | 67.3 / 147.3 ms | http-c lower |

http-c delivers ~2× the throughput at roughly half the CPU cost per
request and lower latency, while using ~8 KB less heap under load.
esp_http_server retains a larger absolute free-heap headroom because the
http-c server object embeds its connection/event-loop tables statically.

### Connection sweep — http-c

| Conns | req/s | avg ms | p50 | p90 | p99 | max | errors |
|------:|------:|-------:|----:|----:|----:|----:|-------:|
| 1  | 90.6  | 10.87 | 9.64  | 16.69 | 25.68 | 41.31 | 0 |
| 2  | 107.9 | 18.48 | 17.31 | 29.06 | 42.35 | 66.68 | 0 |
| 4  | 122.7 | 32.12 | 31.52 | 49.90 | 63.90 | 89.10 | 0 |
| 8  | 120.4 | 33.04 | 32.71 | 51.90 | 66.07 | 94.05 | 4 |
| 16 | 122.3 | 32.42 | 31.74 | 51.23 | 66.78 | 75.78 | 12 |

### Connection sweep — esp_http_server

| Conns | req/s | avg ms | p50 | p90 | p99 | max | errors |
|------:|------:|-------:|----:|----:|----:|----:|-------:|
| 1  | 15.9 | 63.03 | 60.98 | 73.23 | 90.41  | 173.30 | 0 |
| 2  | 34.3 | 57.77 | 56.43 | 66.48 | 82.62  | 97.34  | 0 |
| 4  | 55.3 | 71.49 | 67.26 | 90.12 | 147.28 | 180.61 | 0 |
| 8  | 58.5 | 68.10 | 65.22 | 82.79 | 107.63 | 137.11 | 4 |
| 16 | 56.7 | 70.90 | 68.40 | 87.37 | 112.77 | 150.29 | 12 |

Beyond the 4-connection cap both servers reject excess connections
(reported as `errors`); throughput plateaus rather than scaling, as
expected for the pinned slot count.

## ESP-IDF v6 compatibility fixes

http-c's upstream ESP-IDF example targets IDF v5.x and would not run on
the v6 toolchain installed here. Two issues were fixed in the benchmark
firmware; both are configuration-level and keep the project building on
v5.4 and v6:

1. **Main-task stack overflow.** `http_server_t` embeds an event-loop
   descriptor table (~20 KB). Declaring the server as a stack local in
   `app_main` overflowed the 12 KB main task stack and corrupted the
   heap, faulting the first `nvs_flash_init()` allocation. Fix: the
   server instance is `static` (off the stack) and
   `CONFIG_ESP_MAIN_TASK_STACK_SIZE` is set to 32768.
2. **Wi-Fi bring-up.** ESP-IDF v6's `protocol_examples_common`
   `example_connect()` aborted inside `esp_wifi_init` on this build.
   Replaced with a small self-contained station helper,
   [`firmware-*/main/bench_wifi.c`](firmware-httpc/main/bench_wifi.c)
   (connect + auto-retry, blocks for first IP).

## Reproducing

```sh
. "$HOME/esp/esp-idf/export.sh"

# Set the Wi-Fi SSID/password (password is never committed):
cd firmware-httpc && idf.py menuconfig   # Example Connection Configuration
cd ../firmware-esp && idf.py menuconfig

# Build, flash and run the sweep for each firmware:
./run.sh httpc /dev/ttyUSB0    # auto-detects device IP from the boot log
./run.sh esp   /dev/ttyUSB0

# Render the comparison table:
python3 compare.py results-httpc.json results-esp.json
```

Raw per-run metrics are in [`results-httpc.json`](results-httpc.json)
and [`results-esp.json`](results-esp.json).
