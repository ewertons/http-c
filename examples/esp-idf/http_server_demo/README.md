# http-c on ESP-IDF

A minimal ESP-IDF application that runs the **http-c** library as a plain-HTTP
(port 80) server on an ESP32-class chip. It answers `GET /` with a small HTML
page.

This example doubles as the build target exercised by the `ci-esp32` GitHub
Actions workflow, which proves that the library compiles and links against
ESP-IDF / lwIP / mbedTLS.

## Why plain HTTP?

ESP-IDF ships **mbedTLS**, not OpenSSL, so the http-c component is built with
`SOCKET_TLS_NONE` (TLS disabled). The portable `select()` event-loop backend is
selected as well, because lwIP provides neither `epoll` nor `eventfd`. Both
switches are applied automatically by the repository's root `CMakeLists.txt`
when it is consumed as an ESP-IDF component (see the `if(ESP_PLATFORM)` block).

## Prerequisites

- ESP-IDF v5.x installed and exported (`. $IDF_PATH/export.sh`).
- The http-c repository checked out **with submodules**:

  ```sh
  git clone --recurse-submodules <repo-url>
  # or, in an existing checkout:
  git submodule update --init --recursive
  ```

## Configure, build, flash

```sh
cd examples/esp-idf/http_server_demo
idf.py set-target esp32          # or esp32s3, esp32c3, ...
idf.py menuconfig                # set Wi-Fi SSID/password under
                                 # "Example Connection Configuration"
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

After the device joins the network it logs the assigned IP. Browse to
`http://<device-ip>/` to see the page.

## How it is wired up

- The project's top-level `CMakeLists.txt` adds the repository root (three
  levels up) to `EXTRA_COMPONENT_DIRS`. The root `CMakeLists.txt` registers
  itself as an ESP-IDF component named `http-c`, compiling both the http-c
  core and its bundled `common-lib-c` runtime, and using the microcontroller
  storage preset (`http_server_storage_get_for_microcontroller`).
- Network bring-up uses ESP-IDF's `protocol_examples_common` helper, also
  added to `EXTRA_COMPONENT_DIRS` from `$IDF_PATH`.
