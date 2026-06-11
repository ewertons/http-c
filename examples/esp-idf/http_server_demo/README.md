# http-c on ESP-IDF

A minimal ESP-IDF application that runs the **http-c** library as a plain-HTTP
(port 80) server on an ESP32-class chip. It answers `GET /` with a small HTML
page.

This example doubles as the build target exercised by the `ci-esp32` GitHub
Actions workflow, which proves that the library compiles and links against
ESP-IDF / lwIP / mbedTLS.

## Why plain HTTP?

ESP-IDF ships **mbedTLS**, not OpenSSL, so http-c is built with
`SOCKET_TLS_NONE` (TLS disabled). The portable `select()` event-loop backend is
selected as well, because lwIP provides neither `epoll` nor `eventfd`. Both
switches are exported (PUBLIC) by the `common-lib-c` ESP-IDF component, so
everything that links against it builds with the same configuration.

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

The library is consumed through two **reusable, standalone ESP-IDF
components** that live in the library repositories themselves (not in this
sample), so any ESP-IDF project can use them the same way:

- `http-c` &rarr; `<repo>/components/http-c` &mdash; compiles the http-c core
  with the microcontroller storage preset
  (`http_server_storage_get_for_microcontroller`). It declares
  `REQUIRES common-lib-c`.
- `common-lib-c` &rarr; `<repo>/deps/common-lib-c/components/common-lib-c`
  &mdash; compiles the portable common-lib-c runtime and exports the
  `SOCKET_TLS_NONE` / `EVENT_LOOP_BACKEND_SELECT` switches.

The sample's top-level `CMakeLists.txt` adds both component directories to
`EXTRA_COMPONENT_DIRS`, and the `main` component then simply does
`REQUIRES http-c`. To reuse the library in your own project, add the same two
directories to your `EXTRA_COMPONENT_DIRS` and list `http-c` (and/or
`common-lib-c`) in your component's `REQUIRES`.

Network bring-up uses ESP-IDF's `protocol_examples_common` helper, also added
to `EXTRA_COMPONENT_DIRS` from `$IDF_PATH`.
