# http-c vs esp_http_server — ESP32 benchmark

A head-to-head comparison of [ewertons/http-c](https://github.com/ewertons/http-c)
and ESP-IDF's built-in `esp_http_server`, both serving an identical workload on
a real ESP32. Both firmwares answer:

| Route | Response |
|---|---|
| `GET /` | `hello\n` (6 bytes, `text/plain`, plain HTTP, port 80) |
| `GET /stats` | JSON: `free_heap`, `min_free_heap`, `uptime_ms`, `requests` |

Same chip, same Wi-Fi bring-up (`protocol_examples_common`), same `sdkconfig.defaults`,
4 connection slots each. The only variable is the HTTP server library.

## Layout
- `firmware-httpc/` — ESP-IDF app using http-c (microcontroller storage preset)
- `firmware-esp/`   — ESP-IDF app using `esp_http_server` (`max_open_sockets=4`)
- `loadgen.py`  — dependency-free async HTTP/1.1 keep-alive load generator
- `measure.py`  — runs a 1→16 connection sweep, polls `/stats`, writes results JSON
- `compare.py`  — diffs two results files into a Markdown table
- `run.sh`      — build + flash + measure one firmware end-to-end

## One-time setup
```bash
. ~/esp/esp-idf/export.sh          # alias: esp-idf-init
idf.py menuconfig                  # set Wi-Fi SSID/pass under "Example Connection Configuration" (run in each firmware dir)
```

## Run
```bash
# Flash http-c, note device IP from monitor, then measure
./run.sh httpc 192.168.1.50 /dev/ttyUSB0
# Flash esp_http_server on the same board/network, measure
./run.sh esp   192.168.1.50 /dev/ttyUSB0
# Combine
python3 compare.py results-httpc.json results-esp.json
```

## Metrics collected
- **Throughput & latency** — `best_rps`, p50/p90/p99/max ms across the conn sweep
- **Free / peak heap** — `free_heap` baseline vs `peak_min_free_heap` under load
- **Flash / binary size** — `size-*.txt` from `idf.py size`
- **CPU per request** — `cpu_us_per_req_est` ≈ (2 cores × 1e6) / saturation rps
- **Concurrent capacity** — sweep shows error onset past the 4-slot limit

## Build-time results (esp32, IDF 6.0, this machine)
| Metric | http-c | esp_http_server |
|---|---|---|
| App binary | ~808 KB | ~811 KB |
| Static DRAM | 49.5 KB | 36.2 KB |
| Static IRAM | 87.6 KB | 87.7 KB |

http-c trades ~13 KB more static DRAM (pre-allocated slot buffers, zero malloc
per request) for nearly identical flash; runtime heap/RPS come from a board run.

## Fairness notes
- Plain HTTP only (ESP-IDF ships mbedTLS; http-c builds with TLS off).
- Both pinned to 4 connections and 240 MHz for repeatable numbers.
- Place the host on wired Ethernet near the AP; Wi-Fi RF is the usual bottleneck.
