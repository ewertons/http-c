# API review: Mongoose (cesanta/mongoose)

This document captures how Mongoose's HTTP/HTTPS server API is obtained,
configured, and used, as observed while building the benchmark peer in
[benchmarks/mongoose_bench.c](../benchmarks/mongoose_bench.c). It is
written from the perspective of someone embedding Mongoose in a C
application on Linux/POSIX, with OpenSSL as the TLS backend.

- Upstream:   <https://github.com/cesanta/mongoose>
- License:    GPLv2 (commercial license also available)
- Version sampled: commit `609a0bd` (master, 2025)
- Distribution: a single `mongoose.c` + `mongoose.h` amalgamation

## 1. Getting the source

Mongoose ships as a "header-and-one-source-file" amalgamation, which is
the simplest possible drop-in form for a C codebase:

```sh
git clone --depth 1 https://github.com/cesanta/mongoose external/mongoose
# Files of interest:
#   external/mongoose/mongoose.h
#   external/mongoose/mongoose.c
```

There is no separate `make install` step. You compile `mongoose.c` as
part of your project and `#include "mongoose.h"` from your code.

## 2. Build-time configuration

All build-time configuration is done with C macros, either at the
compiler command line (preferred for embedders) or by editing
`mongoose_config.h`. The macros that matter most for an embedded HTTPS
server:

| Macro              | Value used in the bench           | Purpose                              |
|--------------------|-----------------------------------|--------------------------------------|
| `MG_ARCH`          | `MG_ARCH_UNIX`                    | Selects the POSIX/BSD-sockets backend |
| `MG_TLS`           | `MG_TLS_OPENSSL`                  | Selects OpenSSL as the TLS provider  |
| `MG_ENABLE_OPENSSL`| `1`                               | Pulls in the OpenSSL adapter         |
| `MG_ENABLE_LINES`  | `1`                               | Keeps `#line` directives for debug   |

Other supported `MG_TLS` values include `MG_TLS_BUILTIN` (Mongoose's own
TLS 1.3 stack), `MG_TLS_MBED`, and `MG_TLS_NONE`. The selection is
exclusive — exactly one TLS backend is compiled in per build.

### CMake snippet (matches the bench harness)

```cmake
add_executable(mongoose_bench
    mongoose_bench.c
    external/mongoose/mongoose.c)

target_compile_definitions(mongoose_bench PRIVATE
    MG_ARCH=MG_ARCH_UNIX
    MG_TLS=MG_TLS_OPENSSL
    MG_ENABLE_OPENSSL=1
    MG_ENABLE_LINES=1)

target_include_directories(mongoose_bench PRIVATE
    external/mongoose)

target_link_libraries(mongoose_bench PRIVATE
    OpenSSL::SSL OpenSSL::Crypto)
```

## 3. Core data model

Mongoose exposes only three concepts to the embedder:

- **`struct mg_mgr`** — the event manager. Holds a singly-linked list of
  active connections. There is no thread pool; everything runs from
  `mg_mgr_poll`.
- **`struct mg_connection`** — one TCP connection (listener or
  accepted/outbound). Carries `recv` / `send` byte buffers and a
  per-connection `void *fn_data` user pointer.
- **`mg_event_handler_t`** — a callback of the shape
  `void handler(struct mg_connection *c, int ev, void *ev_data)`.

All control flow is funnelled through that single callback. Events are
plain integers (`MG_EV_OPEN`, `MG_EV_ACCEPT`, `MG_EV_HTTP_MSG`,
`MG_EV_CLOSE`, `MG_EV_ERROR`, ...). For HTTP servers the only event
that practically matters is `MG_EV_HTTP_MSG`, which fires once per
fully-parsed request; `ev_data` then points at a `struct mg_http_message`.

## 4. Minimal HTTPS server (what the bench uses)

```c
#include "mongoose.h"

static const char *s_listen_url = "https://0.0.0.0:8444";
static const char *s_cert_path  = "/work/build/certs/server-cert.pem";
static const char *s_key_path   = "/work/build/certs/server-key.pem";
static const char  s_body[]     = "hello\n";

static void cb(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_ACCEPT && c->is_listening == 0) {
        struct mg_tls_opts opts = {
            .cert = mg_file_read(&mg_fs_posix, s_cert_path),
            .key  = mg_file_read(&mg_fs_posix, s_key_path),
        };
        mg_tls_init(c, &opts);
    } else if (ev == MG_EV_HTTP_MSG) {
        mg_http_reply(c, 200,
                      "Content-Type: text/plain\r\n",
                      "%s", s_body);
    }
}

int main(void) {
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    mg_http_listen(&mgr, s_listen_url, cb, NULL);
    for (;;) mg_mgr_poll(&mgr, 1000 /* ms */);
    mg_mgr_free(&mgr);
    return 0;
}
```

Key things to note:

1. **Listener URL parsing**. `mg_http_listen` takes the full URL form
   (`http://...` / `https://...`). The scheme implicitly arms the
   listener to expect TLS, but does not configure the certificate —
   that is the `MG_EV_ACCEPT` callback's job.
2. **Per-accept TLS init**. `mg_tls_init` must be called on every
   accepted connection (not on the listener). For a fixed cert+key it
   is convenient to pre-read them once at startup and reuse the
   `mg_str` views; the bench does the simple thing and reads on accept.
3. **Reply helper**. `mg_http_reply(c, status, headers, fmt, ...)`
   writes the status line, the supplied headers, an automatic
   `Content-Length`, a blank line, and the formatted body. It is the
   one-shot equivalent of writing into `c->send`.
4. **Single-threaded**. `mg_mgr_poll` iterates every connection,
   running their socket I/O and dispatching events. There is no place
   in the public API for a worker thread; you scale by running multiple
   processes behind `SO_REUSEPORT` or a load balancer.

## 5. Routing

Mongoose has no "router" object. `MG_EV_HTTP_MSG` gives you the parsed
request and you dispatch yourself with `mg_match` / `mg_strcmp`:

```c
if (mg_match(hm->uri, mg_str("/api/echo"), NULL)) { ... }
else if (mg_match(hm->uri, mg_str("/api/users/*"), caps)) { ... }
else { mg_http_reply(c, 404, "", "not found"); }
```

`mg_http_serve_dir` and `mg_http_serve_file` are provided for static
content. `mg_http_get_var` / `mg_http_get_header` cover the usual
parameter / header inspection.

## 6. Memory and lifecycle model

- `mg_mgr_init` does not allocate; `mg_mgr_free` is a no-op except for
  walking the connection list.
- Connections are heap-allocated by `mg_*` connection-creating helpers.
  They live until either the peer closes, the user sets
  `c->is_closing = 1`, or the manager is freed.
- `c->recv` is a growing byte buffer; once `MG_EV_HTTP_MSG` fires you
  may parse from it but must not free it. Mongoose drains the consumed
  request bytes after the handler returns.
- `c->send` is the outbound buffer. `mg_send`, `mg_printf`, and the
  `mg_http_*` helpers append to it. The actual `write(2)` happens
  inside the next `mg_mgr_poll` tick.
- User data is a single `void *fn_data` per connection. You typically
  store a pointer to your application context.

## 7. TLS specifics on Linux

When built with `MG_TLS=MG_TLS_OPENSSL`, the OpenSSL adapter:

- Uses `SSL_CTX_new(TLS_method())` per `mg_tls_init` call (not a shared
  CTX). For high connection rates a custom adapter or pre-built CTX is
  worth considering.
- Enables peer certificate verification only if `opts.ca` is set;
  otherwise the server accepts any client (typical for public HTTPS).
- Drives the handshake via `SSL_read` / `SSL_write` returning
  `WANT_READ` / `WANT_WRITE`, which the manager translates into normal
  poll loop iterations.

## 8. Strengths

- **Single-file drop-in.** No build system required of the embedder.
- **Tiny API surface.** A working server fits on one screen.
- **One unified callback.** Whatever you would do with `epoll` /
  `select` / `mqtt` / `ws` / `http` is reachable from one handler.
- **Portability.** The same source compiles on Linux, FreeRTOS, ESP32,
  Zephyr, and bare-metal NXP/STM32 targets.

## 9. Limitations / things to watch

- **Heap-driven request and response buffers** grow on demand, so
  worst-case memory is hard to bound a priori. Embedded users typically
  set `MG_IO_SIZE` and rely on `c->is_closing = 1` for back-pressure.
- **No structured router**, so URL dispatch is hand-rolled. Easy to
  evolve into a long if/else chain.
- **Single-threaded by design.** Multi-core scaling is the embedder's
  problem (process replicas + `SO_REUSEPORT`).
- **GPLv2 by default.** Embedding into closed-source firmware requires
  the commercial license.

## 10. Comparison to http-c (this repository)

| Concern             | Mongoose                         | http-c                                       |
|---------------------|----------------------------------|----------------------------------------------|
| API style           | one event callback per conn      | route table + per-route handler              |
| Storage             | malloc-grown per-conn buffers    | caller-owned static slot pool                |
| Back-pressure       | `c->is_closing` after-the-fact   | listener unregistered when slot pool full    |
| TLS lifecycle       | `mg_tls_init` per accept         | one `SSL_CTX` for the server, `SSL_new` per slot |
| Routing             | hand-rolled via `mg_match`       | endpoint table keyed by `(method, path)`     |
| Threading           | single-threaded `mg_mgr_poll`    | single-threaded epoll event loop             |
| LoC for hello world | ~25                              | ~60                                          |

The two libraries occupy similar architectural niches but make opposite
choices on memory ownership: Mongoose grows what it needs, http-c
demands you size the slot pool up front and refuses anything beyond it.
